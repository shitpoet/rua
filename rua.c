// rua desktop daemon, AGPL3
// requires SDL2

// cpu load
// rua - 7, 8, 9 %
// pa - 23, 26, 29 %

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdbool.h>
#include <sys/types.h>
#include <tgmath.h>
#include <errno.h>
#include <sys/mman.h>
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
// we use real packet size but still have some upper limit
//#define MAX_PLAY_FRAME 32768
//#define MAX_PLAY_FRAME 16384
#define MAX_PLAY_FRAME 8192

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

bool use_static_remote_ip = false;
struct sockaddr_in static_peer_addr;

bool has_active_peer = false;
struct sockaddr_in active_peer_addr;
socklen_t known_client_addr_len = sizeof(active_peer_addr);

struct sockaddr_in bcast_client_addr;

//uint32_t last_receive_time = 0;

uint64_t send_num = 0;
//uint8_t send_buf[8 + REC_FRAME];
//uint8_t recv_buf[MAX_PLAY_FRAME];
uint8_t* send_buf; // allocated in main with `alloc_buffer`
uint8_t* recv_buf; // allocated in main with `alloc_buffer`


int in_samples_to_ms(int size) {
  return size / PLAY_CHANNELS * 1000 / FREQ;
}

int in_bytes_to_ms(int size) {
  return in_samples_to_ms(size / (BPS / 8));
}

int out_samples_to_ms(int size) {
  return size / REC_CHANNELS * 1000 / FREQ;
}

int out_bytes_to_ms(int size) {
  return out_samples_to_ms(size / (BPS / 8));
}

void* alloc_buffer(size_t size) {
  void* ptr = mmap(NULL, size, PROT_READ | PROT_WRITE,
    MAP_PRIVATE | MAP_ANONYMOUS | MAP_LOCKED, -1, 0);
  if (ptr == MAP_FAILED)
    return NULL;
  return ptr;
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
      sleep_ms(in_bytes_to_ms(frame_size) / 2);
      received = recvfrom(sockfd, buf, max_buf_size,
        MSG_PEEK | MSG_DONTWAIT,
        //(struct sockaddr *)&temp_client_addr, &temp_client_addr_len
        NULL, NULL // clear packets from any sources
      );
      printf("dequeue peek result %ld \n", received);
      //if (received > 4) {
      if (received > 8) {
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

int audio_packets_received = 0;
int play_lost_packets = 0;
int play_underruns = 0;
int play_overruns = 0;

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

  int prev_recv_delta = 0;

  ssize_t prev_received = 0;
  int recv_frame_size = -1;


  #define MAX_SILENCE_SIZE 8192
  uint8_t silence[MAX_SILENCE_SIZE];
  memset(silence, 0, MAX_SILENCE_SIZE);

  SDL_PauseAudioDevice(play_dev, false);

  while (true) {
    //memset(buf, 0, PLAY_FRAME);
    if (use_static_remote_ip) {
      //printf("recvfrom: use static peer address \n");
      temp_client_addr.sin_addr = static_peer_addr.sin_addr;
      if (paused) {
        //SDL_PauseAudioDevice(play_dev, false);
        paused = false;
      }
    } else if (has_active_peer) {
      //printf("recvfrom: use active peer address \n");
      temp_client_addr.sin_addr = active_peer_addr.sin_addr;
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

    // hack ignore x.x.1.x packets to avoid loopback
    /*if (((temp_client_addr.sin_addr.s_addr >> 16) & 0xff) == 1) {
      printf("ignore loopback hack\n");
      continue;
    }*/
    int tpa_a = (temp_client_addr.sin_addr.s_addr >> 0) & 0xff;
    int tpa_b = (temp_client_addr.sin_addr.s_addr >> 8) & 0xff;
    int tpa_c = (temp_client_addr.sin_addr.s_addr >> 16) & 0xff;
    int tpa_d = (temp_client_addr.sin_addr.s_addr >> 24) & 0xff;
    if (tpa_a == 127 && tpa_b == 0 && tpa_c == 0) {
      //printf("ignore 127.0.0.x loopback packets \n");
      continue;
    }

    //if (received == PLAY_FRAME) { // normal packet
    //if (((temp_client_addr.sin_addr.s_addr & 0xff) == 1) && received > 4) { // normal packet
    //if (received > 4) { // normal packet
    if (received > 8) { // normal packet

      uint32_t now = SDL_GetTicks();
      int delta = now - last_receive_time;
      receive_time_sum += delta;
      receive_time_n++;
      if (receive_time_n == 1000) {
        receive_time_sum /= 2;
        receive_time_n /= 2;
      }

      audio_packets_received++;

      if (!has_active_peer || audio_packets_received == 1) {
      //{
        char str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &(temp_client_addr.sin_addr), str, INET_ADDRSTRLEN);
        printf("packet received from %s : ", str);
        printf("%d bytes\n", (int)received);
      }

      uint8_t seq = (uint8_t)recv_buf[0];

      if (seq != (uint8_t)(prev_seq + 1)) {
        if (has_active_peer) {
          int loss;
          if (seq > prev_seq) {
            loss = seq - (prev_seq + 1);
          } else {
            loss = seq + 256 - (prev_seq + 1);
          }
          printf("note: sequence interrupted %d -> %d (%d packets lost) \n", prev_seq, seq, loss);
          if (audio_packets_received >= 1000) play_lost_packets += loss;
        }
      }

      if (received != prev_received) {
        recv_frame_size = received;
        update_udp_recieve_buffer_size(received);
        prev_received = received;
      }

      // save the address for sending
      if (!has_active_peer || active_peer_addr.sin_addr.s_addr != temp_client_addr.sin_addr.s_addr) {
        char str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &(temp_client_addr.sin_addr), str, INET_ADDRSTRLEN);
        printf("new peer %s \n", str);
      }

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

      //char str1[INET_ADDRSTRLEN];
      //inet_ntop(AF_INET, &(temp_client_addr.sin_addr), str1, INET_ADDRSTRLEN);
      //char str2[INET_ADDRSTRLEN];
      //inet_ntop(AF_INET, &(static_peer_addr.sin_addr), str2, INET_ADDRSTRLEN);
      //printf("temp_client_addr %s   static_peer_addr %s \n", str1, str2);

      if (use_static_remote_ip) {
        if (temp_client_addr.sin_addr.s_addr == static_peer_addr.sin_addr.s_addr) {
          active_peer_addr.sin_addr = static_peer_addr.sin_addr;
          has_active_peer = true;
        } else {
          char str1[INET_ADDRSTRLEN];
          inet_ntop(AF_INET, &(temp_client_addr.sin_addr), str1, INET_ADDRSTRLEN);
          char str2[INET_ADDRSTRLEN];
          inet_ntop(AF_INET, &(static_peer_addr.sin_addr), str2, INET_ADDRSTRLEN);
          printf("peer address mismatch: %s != %s \n", str1, str2);
          continue;
        }
      } else {
        active_peer_addr.sin_addr = temp_client_addr.sin_addr;
        has_active_peer = true;
      }
      //printf("bef audio queue size: %d \n", SDL_GetQueuedAudioSize(play_dev));

      int ideal_delta = in_bytes_to_ms(received - 8);
      if (abs(delta + prev_recv_delta - ideal_delta * 2) >= 5 * 2) {
        //printf("%d - %d %d - %d // ", received - 8, delta, prev_recv_delta, ideal_delta * 2);
        printf("[seq %3d] recvd %4ld pdlt %4d ms dlt %4d ms avg %4.2f ms  ", seq, received, prev_recv_delta, delta, (float)receive_time_sum / receive_time_n);
        {
          int queue_size = SDL_GetQueuedAudioSize(play_dev);
          printf("before-queue %4d  \n", queue_size);
        }//*/
      }
      prev_recv_delta = delta;

      if (seq != prev_seq) { // ignore dups

        // skip packet if `recvfrom` took too long
        // it can mean that the packet is already "old"
        // note: I am not sure if it helps
        //if (now - recv_start_time <= 3 * in_bytes_to_ms(recv_frame_size)) {

          int prev_queue_size = SDL_GetQueuedAudioSize(play_dev);
          if (prev_queue_size == 0) {
            if (audio_packets_received >= 1000) play_underruns++;
            //printf("\n");
            //if (delta <= ((received - 8) * 1000) / 2 / FREQ + 5) { // + 5 ms gap
              // delta is small, so we don't anticipate burst of packets
              // because burst is usually big-delta and then several tiny-deltas
              // in this case lets add a little bit of silence delay
              // in an attempt to stabilize the playback
              //int silence_size = (received - 8) / 8;
            //int silence_size = PLAY_SAMPLES * 2;
            int silence_size = PLAY_SAMPLES / 2;
            int silence_ms = silence_size * 1000 / 2 / FREQ;
            printf("playback queue empty - add %d ms of silence ", silence_ms);
            printf("(delta %u <= %ld ms limit)\n", delta, ((received - 8) * 1000) / 2 / FREQ + 5);
            int queue_res = SDL_QueueAudio(play_dev, silence, silence_size);
            /*} else {
              printf("playback queue empty %d \n", seq);
            }*/
          }

          // do not enqueue if the queue already has
          // significant number of packets
          if (prev_queue_size / recv_frame_size <= 4) {
            int queue_res = SDL_QueueAudio(play_dev, play_buf, received - 8);
            //int queue_res = 0;
            if (queue_res != 0) {
              //printf("dev id: %d \n", play_dev);
              //printf("\n");
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

            /*printf("after-queue %4d  ", queue_size);
            //printf("%4d sf: ", queue_size / 1440);
            for (int i = 0; i < queue_size / received; i++) {
              printf("q");
            }
            printf("\n");//*/

            if (prev_queue_size == 0) {
              //printf("playback queue was empty\n");
            }
            //printf("\n");
          } else {
            //printf("\n");
            printf("playback queue is full, drop incoming packet seq %d \n", seq);
            if (audio_packets_received >= 1000) play_overruns++;
          }
        /*} else {
          printf("skip late incoming packet \n");
          //empty_recv_udp_queue(sockfd, buf, MAX_PLAY_FRAME, recv_frame_size);
        }*/
      } else {
        //printf("\n");
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

      if (audio_packets_received >= 2000 && audio_packets_received % 1000 == 0) {
        printf("play stats: %d ms of lost packets (%.2f%%), %d underruns (%.2f%%), %d overruns (%.2f%%)\n",
          play_lost_packets * in_bytes_to_ms(received - 8),
          play_lost_packets * 100.0 / (audio_packets_received - 1000),
          play_underruns, play_underruns * 100.0 / (audio_packets_received - 1000),
          play_overruns, play_overruns * 100.0 / (audio_packets_received - 1000)
        );
      }

    } else if (received == 4) { // broadcast packet
      if (use_static_remote_ip) {
        printf("ignore boradcast packets in static remote mode \n");
      } else {
        // do nothing if local loopback
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
          active_peer_addr.sin_addr = temp_client_addr.sin_addr;
          has_active_peer = true;
        }
      }
    } else if (received > 0) {
      printf("unrecognized packet received, size: %d bytes \n", (int)received);
    } else if (received == 0) { // shutdown
      // do nothing
      has_active_peer = false;
      printf("remote side shutdown \n");
    } else { // error
      has_active_peer = false;
      //fprintf(stderr, "udp broadcast send errno %d \n", errno);
      fprintf(stderr, "udp recvfrom send errno %s \n", strerror(errno));
    }
  }
}

uint32_t last_rec_callback_time;
//int rec_num = 0;
uint64_t rec_cb_time_sum = 0;
uint64_t rec_cb_n = 0;

void rec_callback(void* userdata, uint8_t* buf, int recorded) {
  //printf("rec_callback len %9d \n", recorded);
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
    if (has_active_peer) {
      // have known client ip address
      // send audio packet
      //memset(send_buf, 0, sizeof(send_buf));

      uint32_t send_start = SDL_GetTicks();

      //printf("send-to-known-peer: %d bytes, delta %3d ms, avg %3f ms,  send_time %3d ms \n", recorded, delta, avg_delta, -1);
      {
        char str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &(active_peer_addr.sin_addr), str, INET_ADDRSTRLEN);
        //printf("sendto audio %s \n", str);
      }

      // copy buffer because we need to add header to it
      *((uint64_t*)send_buf) = send_num++;
      memcpy(send_buf + 8, buf, REC_FRAME);
      int sent = sendto(sockfd,
        (const char*)send_buf, 8 + REC_FRAME,
        MSG_CONFIRM, // sending to known client
        (const struct sockaddr*)&active_peer_addr,
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
        if (use_static_remote_ip) {
          printf("send pseudo-broadcast to static remote ip \n");
          *((uint32_t*)send_buf) = app_id;
          int sent = sendto(sockfd, (const char*)send_buf, sizeof(app_id),
            0,
            (const struct sockaddr*)&static_peer_addr, sizeof(static_peer_addr)
          );
          if (sent == -1) {
            fprintf(stderr, "udp pseudo-broadcast send errno %s \n", strerror(errno));
          }
        } else {
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
      //SDL_PauseAudioDevice(rec_dev, false);
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
  printf("hi\n");

  send_buf = alloc_buffer(8 + REC_FRAME);
  recv_buf = alloc_buffer(MAX_PLAY_FRAME);

  struct sockaddr_in server_addr;
  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET; // ipv4
  server_addr.sin_addr.s_addr = INADDR_ANY;
  server_addr.sin_port = htons(PORT);

  memset(&static_peer_addr, 0, sizeof(static_peer_addr));
  static_peer_addr.sin_family = AF_INET; // ipv4
  //static_peer_addr.sin_addr.s_addr = <not known yet>;
  static_peer_addr.sin_port = htons(PORT);

  memset(&active_peer_addr, 0, sizeof(active_peer_addr));
  active_peer_addr.sin_family = AF_INET; // ipv4
  //active_peer_addr.sin_addr.s_addr = <not known yet>;
  active_peer_addr.sin_port = htons(PORT);

  memset(&bcast_client_addr, 0, sizeof(bcast_client_addr));
  bcast_client_addr.sin_family = AF_INET; // ipv4
  bcast_client_addr.sin_addr.s_addr = INADDR_BROADCAST;
  bcast_client_addr.sin_port = htons(PORT);

  char* bind_dev = NULL;

  int num_args = argc - 1;
  if (num_args == 0) {
    // find peer ip using broadcast
  } else if (num_args == 1) {
    char* remote_ip = argv[1];
    printf("static remote ip %s \n", remote_ip);
    use_static_remote_ip = true;
    inet_pton(AF_INET, remote_ip, &(static_peer_addr.sin_addr));
  } else if (num_args == 2) {
    bind_dev = argv[1];
    printf("bind dev %s \n", bind_dev);
    char* remote_ip = argv[2];
    printf("static remote ip %s \n", remote_ip);
    use_static_remote_ip = true;
    inet_pton(AF_INET, remote_ip, &(static_peer_addr.sin_addr));
  } else {
    printf("usage: rua \n");
    printf("       finds an ip using broadcast\n");
    printf("usage: rua remote-ip-address\n");
    printf("       uses provided ip address\n");
    printf("usage: rua bind-dev remote-ip-address\n");
    printf("       uses device and provided ip address\n");
    exit(1);
  }

  srand(time(NULL));
  app_id = rand() + RAND_MAX * (rand() + RAND_MAX * rand());

  SDL_version sdl_ver;
  SDL_GetVersion(&sdl_ver);
  printf("sdl ver %d.%d.%d \n", sdl_ver.major, sdl_ver.minor, sdl_ver.patch);

  //struct sigaction action;
  //sigaction(SIGINT, NULL, &action);
  SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1"); // to avoid capturing of ctrl+c
  if (SDL_Init(SDL_INIT_AUDIO | SDL_INIT_NOPARACHUTE) < 0) sdl_fail("sdl init");
  //sigaction(SIGINT, &action, NULL);
  //sigaction(SIGINT, SIG_DFL, NULL);

  sockfd = socket(AF_INET, SOCK_DGRAM, 0);
  if (sockfd < 0) fail("listening udp socket creation failed");

  if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &(int){1}, sizeof(int)) < 0)
    printf("warn: setsockopt(SO_REUSEADDR) failed\n");
  if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEPORT, &(int){1}, sizeof(int)) < 0)
    printf("warn: setsockopt(SO_REUSEPORT) failed\n");

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

  play_dev = open_play_dev();
  printf("play_dev id %d \n", play_dev);
  rec_dev = open_rec_dev();
  printf("rec_dev id %d \n", rec_dev);

  if (bind(sockfd, (const struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
    fail("udp socket bind failed");
  }
  printf("listening on %d...\n", PORT);

  if (bind_dev != NULL) {
    if (setsockopt(sockfd, SOL_SOCKET, SO_BINDTODEVICE, (void *)bind_dev, strlen(bind_dev) + 1) < 0) {
      printf("warn: failed to bind to interface %s\n", bind_dev);
    }
  }

  pthread_create(&play_thread, NULL, play_thread_func, NULL);
  //SDL_PauseAudioDevice(rec_dev, false);
  pthread_create(&rec_thread, NULL, rec_thread_func, NULL);

  /*while (true) {
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
  }*/

  // note: this code is almost unreachable
  // but still there is some code that can
  // be helpful in the future

  // wait threads to terminate
  pthread_join(play_thread, NULL);
  pthread_join(rec_thread, NULL);

  SDL_CloseAudio();

  return 0;
}

