// rua desktop daemon, GPL3
// requires SDL2

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdbool.h>
#include <sys/types.h>
#include <tgmath.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <SDL2/SDL.h>

//#define LOCAL_PORT 29478
//#define REMOTE_PORT 29479
#define PORT 59100

#define FREQ 48000
#define BPS 16 // bits per sample for s16le

//#define PACKET_HEADER 2

#define PLAY_CHANNELS 1
#define PLAY_SAMPLES 512
#define PLAY_FRAME (PLAY_SAMPLES * (BPS / 8) * PLAY_CHANNELS)
// play frame should be the same as on the phone
// otherwise there will be truncation
//#define PLAY_FRAME (3528 / 2)
//#define MAX_PLAY_FRAME (PACKET_HEADER + 32768)
#define MAX_PLAY_FRAME 32768

#define REC_CHANNELS 1
#define REC_SAMPLES 256
//#define REC_SAMPLES 512
//#define REC_SAMPLES 1024
#define REC_FRAME (REC_SAMPLES * (BPS / 8) * REC_CHANNELS)
#define REC_FRAME_TIME (REC_SAMPLES * 1000 / FREQ)
#define REC_PACKET_SIZE (8 + REC_FRAME)
#define MAX_REC_PACKET_SIZE 32768

pthread_t play_thread;
pthread_t rec_thread;

int rec_dev = -1; // sdl device
int play_dev = -1; // sdl device

uint32_t app_id;
//char* remote_ip;
int sockfd = -1;

bool has_known_client = false;

struct sockaddr_in known_client_addr;
socklen_t known_client_addr_len = sizeof(known_client_addr);

struct sockaddr_in bcast_client_addr;

//uint32_t last_receive_time = 0;

uint64_t send_num = 0;
uint8_t send_buf[8 + REC_FRAME];
uint8_t recv_buf[MAX_PLAY_FRAME];



int in_size_to_ms(int size) {
  return size / PLAY_CHANNELS * 1000 / FREQ;
}

int out_size_to_ms(int size) {
  return size / REC_CHANNELS * 1000 / FREQ;
}

void sleep_ms(int ms) {
  usleep(ms * 1000);
}

uint8_t empty_recv_udp_queue(int sockfd, char* buf, int max_buf_size, int frame_size, uint8_t seq) {
  printf("empty_recv_udp_queue \n");
  if (sockfd != -1) {
    ssize_t received;
    int k = 0;
    do {
      sleep_ms(in_size_to_ms(frame_size) / 2);
      received = recvfrom(sockfd, buf, max_buf_size,
        MSG_PEEK | MSG_DONTWAIT,
        //(struct sockaddr *)&temp_client_addr, &temp_client_addr_len
        NULL, NULL // clear packets from any sources
      );
      printf("dequeue peek result %ld \n", received);
      if (received > 4) {
        // have something ready in the queue
        // and this is not a udp broadcast
        printf("dequeue a udp packet \n");
        // read the packet to dequeue it from the queue
        received = recvfrom(sockfd, buf, max_buf_size,
          0,
          //(struct sockaddr *)&temp_client_addr, &temp_client_addr_len
          NULL, NULL // clear packets from any sources
        );
        seq = buf[0];
      }
      k++;
    } while (k < 4);
  }
  return seq;
}

/*void play_callback(void* userdata, uint8_t* stream, int len) {
	if (play_len == 0) return;

	//len = (len > audio_len ? audio_len : len);
	//memcpy(stream, audio_pos, len);
  int i = 0;
  while (i < len / 2) {
    *stream++ = i && 0xff;
    *stream++ = i >> 8;
    i++;
  }

	//audio_pos += len;
	//audio_len -= len;
}*/

void fail(const char* msg) {
  fprintf(stderr, "error: %s \n", msg);
  exit(1);
}

void sdl_fail(const char* msg) {
  fprintf(stderr, "sdl error: %s (%s) \n", msg, SDL_GetError());
  exit(1);
}

void print_audio_satus(SDL_AudioDeviceID dev)
{
  switch (SDL_GetAudioDeviceStatus(dev))
  {
    case SDL_AUDIO_STOPPED: printf("stopped\n"); break;
    case SDL_AUDIO_PLAYING: printf("playing\n"); break;
    case SDL_AUDIO_PAUSED: printf("paused\n"); break;
    default: printf("???"); break;
  }
}

int open_play_dev() {
	SDL_AudioSpec play_spec;
  SDL_zero(play_spec);
  play_spec.freq = FREQ;
  play_spec.format = AUDIO_S16;
  play_spec.channels = PLAY_CHANNELS;
  play_spec.samples = PLAY_SAMPLES;
	play_spec.callback = NULL;
  int play_dev = SDL_OpenAudioDevice(NULL, 0, &play_spec, NULL, 0);
  if (play_dev == 0) sdl_fail("sdl open output audio device");
  return play_dev;
}

// set minimum network buffers to avoid stacking of them
// note: it may cause packet loss if receiving is too slow
// on in case of some drifts of packets arrival
// note: this function is called on first data packet
// to be allow source to adjust packet granuality
// as it wishes
void update_udp_recieve_buffer_size(int frame_size) {
  printf("update_udp_recieve_buffer_size to %d \n", frame_size);
  int size = frame_size * 2;
  if (setsockopt(sockfd, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size)) == -1)
    fail("can not set network receive (play) buffer size");
}

void* play_thread_func(void* param) {
  // temp addr
  struct sockaddr_in temp_client_addr;
  memset(&temp_client_addr, 0, sizeof(temp_client_addr));
  temp_client_addr.sin_family = AF_INET; // ipv4
  temp_client_addr.sin_port = htons(PORT);
  socklen_t temp_client_addr_len = sizeof(temp_client_addr);

  uint8_t* play_buf = recv_buf + 8;

  // empty input udp queue to avoid potential latency increase
  // note: not sure if it is needed, but just in case
  //empty_recv_udp_queue(sockfd, buf, MAX_PLAY_FRAME);

  bool paused = true;

  //memset(buf, 0, MAX_PLAY_FRAME);
  //SDL_QueueAudio(play_dev, buf, 256 * 2);

  uint32_t last_receive_time = SDL_GetTicks();
  long receive_time_sum = 0;
  long receive_time_n = 0;
  uint8_t prev_seq = -1;

  ssize_t prev_received = 0;
  int recv_frame_size = -1;

  SDL_PauseAudioDevice(play_dev, false);

  while (true) {
    //memset(buf, 0, PLAY_FRAME);
    if (has_known_client) {
      //printf("recvfrom: has_known_client \n");
      temp_client_addr.sin_addr = known_client_addr.sin_addr;
      if (paused) {
        //SDL_PauseAudioDevice(play_dev, false);
        paused = false;
      }
    } else {
      // lets receive packet from any host
      printf("recvfrom: no known client \n");
      temp_client_addr.sin_addr.s_addr = INADDR_ANY;
      if (!paused) {
        //SDL_PauseAudioDevice(play_dev, true);
        paused = true;
      } else {
        // playback is already paused
        // this means we have two failed attempt to `recvfrom`
        // lets clear audio queue just in case in this case
        //SDL_ClearQueuedAudio(play_dev);
        //printf("warn: clear playback audio queue \n");
      }
    }
    //printf("recvfrom \n");
    int recv_start_time = SDL_GetTicks();
    ssize_t received = recvfrom(sockfd, recv_buf, MAX_PLAY_FRAME,
      0,
      //NULL, NULL
      (struct sockaddr *)&temp_client_addr, &temp_client_addr_len
    );
    //ssize_t received = 512;

    //{
      //char str[INET_ADDRSTRLEN];
      //inet_ntop(AF_INET, &(temp_client_addr.sin_addr), str, INET_ADDRSTRLEN);
      //printf("packet received from ip %s : ", str);
      //printf("%d bytes\n", (int)received);
    //}

    //if (received == PLAY_FRAME) { // normal packet
    if (received > 4) { // normal packet

      uint32_t now = SDL_GetTicks();
      uint32_t delta = now - last_receive_time;
      receive_time_sum += delta;
      receive_time_n++;

      uint8_t seq = (uint8_t)recv_buf[0];

      if (seq != (uint8_t)(prev_seq + 1)) {
        printf("sequence interrupted %d -> %d \n", prev_seq, seq);
      }

      if (received != prev_received) {
        recv_frame_size = received;
        update_udp_recieve_buffer_size(received);
        prev_received = received;
      }

      //printf("[seq %3d] recvd dlt %4d ms avg %4f ms  ", seq, delta, (float)receive_time_sum / receive_time_n);
      last_receive_time = now;

      /*printf("\n");
      for (int i = 0; i < received; i += 2) {
        //printf("%3d %3d   ", abs((int8_t)buf[i + 1]), (uint8_t)buf[i]);
        //buf[i + 1] = (int)(31.0 * sin((float)i / 500.0));
        //printf("%3d   ", abs((int8_t)buf[i + 1]));
        //if (i % 16 == 0) printf("\n");
      }
      printf("\n");
      //printf("\n");*/

      // save the address for sending
      known_client_addr.sin_addr = temp_client_addr.sin_addr;
      has_known_client = true;
      //printf("bef audio queue size: %d \n", SDL_GetQueuedAudioSize(play_dev));

      if (seq != prev_seq) { // ignore dups

        // skip packet if `recvfrom` took too long
        // it can mean that the packet is already "old"
        // note: I am not sure if it helps
        //if (now - recv_start_time <= 3 * in_size_to_ms(recv_frame_size)) {

          int prev_queue_size = SDL_GetQueuedAudioSize(play_dev);
          /*if (prev_queue_size == 0) {
            printf("playback queue empty \n");
          }*/

          // do not enqueue if the queue already has
          // significant number of packets
          if (prev_queue_size / recv_frame_size <= 3) {
            int queue_res = SDL_QueueAudio(play_dev, play_buf, received - 8);
            //int queue_res = 0;
            if (queue_res != 0) {
              //printf("dev id: %d \n", play_dev);
              sdl_fail("playback queue error");
            }
            //printf("aft audio queue size: %d \n", );
            int queue_size = SDL_GetQueuedAudioSize(play_dev);
            int latency = queue_size / 2 * 1000 / FREQ;

            //printf("latency %d ms  ", latency);

            /*for (int i = 0; i < (latency + 5) / 10; i++) {
              printf("l");
            }
            printf("\n");*/

            /*printf("queue %d  ", queue_size);
            //printf("%4d sf: ", queue_size / 1440);
            for (int i = 0; i < queue_size / received; i++) {
              printf("f");
            }//*/

            if (prev_queue_size == 0) {
              //printf("playback queue was empty\n");
            }
            //printf("\n");
          } else {
            printf("playback queue is full, drop incoming packet \n");
          }
        /*} else {
          printf("skip late incoming packet \n");
          //empty_recv_udp_queue(sockfd, buf, MAX_PLAY_FRAME, recv_frame_size);
        }*/
      } else {
        printf("skip duplicate incoming packet \n");
      }

      // 255 0 are near in time
      // i do not trust this trick, i believe it can
      // make drops even worse
      // what we need really is timestamp-based packets
      // dropping i think, the problem here is that
      // the clocks are not synchronizied
      /*if (
        abs(seq - prev_seq) > 4 &&
        abs(seq - (256 + prev_seq)) > 4 &&
        abs((256 + seq) - prev_seq) > 4
      ) {
        printf("mass-drop detected \n");
        seq = empty_recv_udp_queue(sockfd, buf, MAX_PLAY_FRAME, recv_frame_size, seq);
      }*/

      prev_seq = seq;

      //char str[INET_ADDRSTRLEN];
      //inet_ntop(AF_INET, &(temp_client_addr.sin_addr), str, INET_ADDRSTRLEN);
      //printf("packet received from ip %s \n", str);

    } else if (received == 4) { // broadcast packet
      // do nothing
      if (
        temp_client_addr.sin_addr.s_addr == INADDR_LOOPBACK ||
        *((uint32_t*)recv_buf) == app_id
      ) {
        printf("ignore own broadcast packet \n");
      } else {
        char str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &(temp_client_addr.sin_addr), str, INET_ADDRSTRLEN);
        printf("broadcast packet received ip %s \n", str);
        // save the address for sending
        known_client_addr.sin_addr = temp_client_addr.sin_addr;
        has_known_client = true;
      }
    } else if (received > 0) {
      printf("unrecognized packet received, size: %d bytes \n", (int)received);
    } else if (received == 0) { // shutdown
      // do nothing
      has_known_client = false;
      printf("remote side shutdown \n");
    } else { // error
      has_known_client = false;
      //fprintf(stderr, "udp broadcast send errno %d \n", errno);
      fprintf(stderr, "udp broadcast send errno %s \n", strerror(errno));
    }
  }
}

uint32_t last_rec_callback_time;
//int rec_num = 0;
uint64_t rec_cb_time_sum = 0;
uint64_t rec_cb_n = 0;

void rec_callback(void* userdata, uint8_t* buf, int recorded) {
  //printf("rec %9d len %9d \n", rec_num++, len);
  //printf("audio recorded: %d \n", recorded);

  uint32_t now = SDL_GetTicks();
  uint32_t delta = now - last_rec_callback_time;
  rec_cb_time_sum += delta;
  rec_cb_n++;
  if (rec_cb_n >= 256) {
    rec_cb_time_sum /= 2;
    rec_cb_n /= 2;
  }
  float avg_delta = (float)rec_cb_time_sum / rec_cb_n;
  //if (delta > REC_SAMPLES * 1000 / FREQ + 1) {
  //printf("rec callback %d bytes, delta %3d ms, avg %3f ms \n", recorded, delta, avg_delta);
  //}
  last_rec_callback_time = now;

  if (recorded == REC_FRAME) {
    if (has_known_client) {
      // have known client ip address
      // send audio packet
      //memset(send_buf, 0, sizeof(send_buf));

      uint32_t send_start = SDL_GetTicks();

      // copy buffer because we need to add header to it
      *((uint64_t*)send_buf) = send_num++;
      memcpy(send_buf + 8, buf, REC_FRAME);
      int sent = sendto(sockfd,
        (const char*)send_buf, 8 + REC_FRAME,
        MSG_CONFIRM, // sending to known client
        (const struct sockaddr*)&known_client_addr,
        known_client_addr_len
      );

      uint32_t now = SDL_GetTicks();
      uint32_t send_time = now - send_start;

      if (delta > REC_FRAME_TIME + 1 && avg_delta > REC_FRAME_TIME + .5 || send_time >= REC_FRAME_TIME / 2) {
        printf("send-to-known-peer: %d bytes, delta %3d ms, avg %3f ms,  send_time %3d ms \n", recorded, delta, avg_delta, send_time);
      }

      if (sent == -1) {
        fprintf(stderr, "udp send errno %s \n", strerror(errno));
      } else if (sent != 8 + recorded) {
        fprintf(stderr, "udp send - not all bytes sent! %d of %d \n", sent, 8 + recorded);
      } else {
        //printf("packet seq %d -> %d bytes sent \n", (int)send_num - 1, sent);
      }

      //*/


    } else {
      //printf("no client connected\n");
      // no client connected
      // brodcast to let all potentical clients
      // to know our address
      // uses tiny byte packet to avoid unnecessary traffic
      if ((send_num++ & 0x0f) == 0) {
        printf("send udp broadcast \n");
        *((uint32_t*)send_buf) = app_id;
        int sent = sendto(sockfd, (const char*)send_buf, sizeof(app_id),
          0,
          (const struct sockaddr*)&bcast_client_addr, sizeof(bcast_client_addr)
        );
        if (sent == -1) {
          fprintf(stderr, "udp broadcast send errno %s \n", strerror(errno));
        }
      }
    }
  } else {
    fprintf(stderr, "recorded less samples than requested %d < %d \n", recorded, REC_FRAME);
  }
}

int open_rec_dev() {
  SDL_AudioSpec rec_spec;
  SDL_zero(rec_spec);
  rec_spec.freq = FREQ;
  rec_spec.format = AUDIO_S16;
  rec_spec.channels = REC_CHANNELS;
  rec_spec.samples = REC_SAMPLES;
  rec_spec.callback = rec_callback;
  int rec_dev = SDL_OpenAudioDevice(NULL, true, &rec_spec, NULL, 0);
  if (rec_dev == 0) sdl_fail("sdl open input audio device");
  return rec_dev;
}

void* rec_thread_func(void* param) {
  SDL_PauseAudioDevice(rec_dev, false);
  while (true) {
    // try to detect stuck recording..
    uint32_t t1 = last_rec_callback_time;
    SDL_Delay(727);
    uint32_t t2 = last_rec_callback_time;
    //printf("%ud %ud \n", t1, t2);
    // this too work from times to times only...
    if (t1 == t2) {
      printf("\n");
      printf("warn: recording seems to be stuck - restart capturing \n");
      SDL_CloseAudioDevice(rec_dev);
      rec_dev = open_rec_dev();
      pthread_create(&rec_thread, NULL, rec_thread_func, NULL);
      printf("new rec_dev id %d \n", rec_dev);
      printf("\n");
      return NULL;
    }
    // no change for hot pulseaudio channel change..
    //printf("rec dev status %d \n", SDL_GetAudioDeviceStatus(rec_dev));
    //SDL_Delay(72711);
    /* // no event for hot pulseaudio channel change..
    SDL_Event event;
    SDL_WaitEvent(&event);
    printf("sdl event %d \n", event.type);
    if (event.type == SDL_AUDIODEVICEREMOVED && event.adevice.iscapture) {
      // note: one should not reopen two devices at the same time
      //       we do not now reopen play dev so it is ok w/o sync
      printf("capture audio device removed - restart capturing\n");
      SDL_CloseAudioDevice(rec_dev);
      rec_dev = open_rec_dev();
      pthread_t rec_thread;
      pthread_create(&rec_thread, NULL, rec_thread_func, NULL);
      printf("rec_dev id %d \n", rec_dev);
      break;
    } else {
      SDL_Delay(50);
    }*/
  }
  /*

  char buf[REC_FRAME];

  while (true) {
    int recorded = SDL_DequeueAudio(rec_dev, buf, REC_FRAME);
    if (recorded > 0) {
      printf("audio recorded: %d \n", recorded);
      int sent = sendto(sockfd, (const char*)buf, recorded,
        MSG_CONFIRM,
        (const struct sockaddr*)&client_addr, client_addr_len
      );
      if (sent > 0) {
        printf("%d bytes sent \n", sent);
      }
      if (sent == -1) {
        fprintf(stderr, "udp send errno %s \n", strerror(errno));
      }
    } else {
      printf("no audio dequeued \n");
    }
  }*/
  return NULL;
}

int main(int argc, char** argv) {
  //if (argc == 1 + 1) {
    //remote_ip = argv[1];
    //printf("remote ip %s \n", remote_ip);
  //} else {
    //printf("usage: rua remote_ip_address\n");
    //exit(1);
  //}

  printf("hi\n");

  srand(time(NULL));
  app_id = rand() + RAND_MAX * (rand() + RAND_MAX * rand());

  SDL_version sdl_ver;
  SDL_GetVersion(&sdl_ver);
  printf("sdl ver %d.%d.%d \n", sdl_ver.major, sdl_ver.minor, sdl_ver.patch);

  struct sigaction action;
  sigaction(SIGINT, NULL, &action);
  if (SDL_Init(SDL_INIT_AUDIO) < 0) sdl_fail("sdl init");
  sigaction(SIGINT, &action, NULL);

  struct sockaddr_in server_addr;

  sockfd = socket(AF_INET, SOCK_DGRAM, 0);
  if (sockfd < 0) fail("listening udp socket creation failed");

  int broadcast_enable = 1;
  if (setsockopt(sockfd, SOL_SOCKET, SO_BROADCAST, &broadcast_enable, sizeof(broadcast_enable)) == -1)
    fail("can not acquire socket broadcast permission");

  // set send timeout to avoid infinite blocking in case of problems
  // the timeout is small because we send in loops at a high freq
  // also we call `send` in record callback and long `send`
  // calls seems to make the SDL to behave in some strange way
  int play_frame_ms = PLAY_FRAME * 1000 / FREQ;
  struct timeval send_timeout = {0, play_frame_ms * 1000 / 2}; // sec, microsec
  if (setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, (char*)&send_timeout, sizeof(send_timeout)) == -1)
    fail("can not set send timeout");

  // set recv timeout to avoid infinite waiting in case of problems
  // the timeout is small because we recv in loops at a high freq
  struct timeval recv_timeout = {1, 0 * 1000}; // sec, microsec
  if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, (char*)&recv_timeout, sizeof(recv_timeout)) == -1)
    fail("can not set receive timeout");

  int rec_frame = REC_PACKET_SIZE * 1;
  if (setsockopt(sockfd, SOL_SOCKET, SO_SNDBUF, &rec_frame, sizeof(rec_frame)) == -1)
    fail("can not set network send (record) buffer size");
  //note: receive socket buf size is set elsewhere

  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET; // ipv4
  server_addr.sin_addr.s_addr = INADDR_ANY;
  server_addr.sin_port = htons(PORT);

  memset(&known_client_addr, 0, sizeof(known_client_addr));
  known_client_addr.sin_family = AF_INET; // ipv4
  //known_client_addr.sin_addr.s_addr = <not known yet>;
  known_client_addr.sin_port = htons(PORT);

  memset(&bcast_client_addr, 0, sizeof(bcast_client_addr));
  bcast_client_addr.sin_family = AF_INET; // ipv4
  bcast_client_addr.sin_addr.s_addr = INADDR_BROADCAST;
  bcast_client_addr.sin_port = htons(PORT);

  play_dev = open_play_dev();
  printf("play_dev id %d \n", play_dev);
  rec_dev = open_rec_dev();
  printf("rec_dev id %d \n", rec_dev);

  if (bind(sockfd, (const struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
    fail("udp socket bind failed");
  }
  printf("listening on %d...\n", PORT);

  pthread_create(&play_thread, NULL, play_thread_func, NULL);
  pthread_create(&rec_thread, NULL, rec_thread_func, NULL);

  while (true) {
    SDL_Event event;
    SDL_WaitEvent(&event);
    if (event.type == SDL_QUIT) {
      break;
    }
    if (event.type == SDL_AUDIODEVICEREMOVED) {
      printf("warn: audio device removed event - restart mb needed \n");
      printf("      but restart is not implemented for this case \n");
    }
    SDL_Delay(51);
  }

  // note: this code is almost unreachable
  // but still there is some code that can
  // be helpful in the future

  // wait threads to terminate
  pthread_join(play_thread, NULL);
  pthread_join(rec_thread, NULL);

  SDL_CloseAudio();

  return 0;
}

