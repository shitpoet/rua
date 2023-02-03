// rua mobile app (Android 8+), GPL3

package cc.shitpoet.rua

import android.Manifest.permission.RECORD_AUDIO
import android.annotation.SuppressLint
import android.content.Context
import android.content.Context.*
import android.content.pm.PackageManager
import android.media.*
import android.net.wifi.WifiManager
import android.net.wifi.WifiManager.MulticastLock
import android.os.Bundle
import android.os.StrictMode
import android.os.StrictMode.ThreadPolicy
import android.util.Log
import androidx.appcompat.app.AppCompatActivity
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat
import java.lang.Math.ceil
import java.lang.Math.floor
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress
import java.util.concurrent.Semaphore


// MIC can have lower latency but may have no echo cancelation
// VC can have higher latency but may do echo cancelation
// VR can be even better on noise cancelation in some cases (I believe)
// const val audioSource = MediaRecorder.AudioSource.MIC
// const val audioSource = MediaRecorder.AudioSource.VOICE_COMMUNICATION
const val audioSource = MediaRecorder.AudioSource.VOICE_RECOGNITION

const val sampleRate = 48000 // note: should be equal to native sample rate
const val port = 59100
const val remoteRecSamples = 256
const val remoteRecFrame = remoteRecSamples * 2 // bytes
const val remoteRecPacketSize = 8 + remoteRecFrame

const val playChannels = 1 // note: only mono is supported right now
const val recChannels = 1 // note: only mono is supported right now

var audioMinOutBufferSize = -1
// var audioOutBufferSize = -1
var audioMinInBufferSize = -1
// var audioInBufferSize = -1

var receiveBufferFragments = 999
var receiveBuffer = ByteArray(1) // dummy array to avoid null
var receiveBufferWritePos = 0
//var receiveBufferWritten = 0
//var receiveBufferReadPos = 0
//var receiveBufferRead = 0

//val playCopyBuffer = ByteArray(remoteRecFrame)

//val receiveBufferQueueSemaphore = Semaphore(-1)
val receiveBufferQueueSemaphore = Semaphore(0) // so the first acquire blocks
// val receiveBufferAccessSmaphore = Semaphore(1)
// val receiveBufferPosSemaphore = Semaphore(1)

var socket: DatagramSocket = DatagramSocket(port)
//var broadcastAddress: InetAddress? = null
//var socket: DatagramSocket = DatagramSocket(port, InetAddress.getByName("192.168.43.1"))
var hasKnownClient = false
var remoteAddress: InetAddress? = null
//var appId = (0..Long.MAX_VALUE).random()
var appId = ByteArray(4) // random, inited on app load

// xiaomi redmi 6a min input buf size 3528
// xiaomi redmi 6a min output buf size 2836

fun setThreadPolicy() {
    val policy = ThreadPolicy.Builder().permitAll().build()
    StrictMode.setThreadPolicy(policy)
}

fun log(s: String) {
    Log.d("rua", s)
}

fun byteArrayToULong(buffer: ByteArray): ULong {
    return (
        (buffer[0].toUByte().toULong() shl (8 * 0)) or
        (buffer[1].toUByte().toULong() shl (8 * 1)) or
        (buffer[2].toUByte().toULong() shl (8 * 2)) or
        (buffer[3].toUByte().toULong() shl (8 * 3)) or
        (buffer[4].toUByte().toULong() shl (8 * 4)) or
        (buffer[5].toUByte().toULong() shl (8 * 5)) or
        (buffer[6].toUByte().toULong() shl (8 * 6)) or
        (buffer[7].toUByte().toULong() shl (8 * 7))
    )
}

// fun powerOfTwo(x: Int): Int {
//     val mask = x - 1
//     val ones = Integer.numberOfLeadingZeros(mask)
//     val bits = Integer.SIZE - ones
//     return 1 shl bits
// }

var avgRecvBurstSize = 3

@Suppress("DEPRECATION")
class PlayThread : Runnable {
    override fun run() {
        log("play thread run")
        setThreadPolicy();
        try {
            assert(playChannels == 1)
            /*val audioTrack = AudioTrack(
                AudioManager.STREAM_MUSIC,
                sampleRate, AudioFormat.CHANNEL_OUT_MONO,
                AudioFormat.ENCODING_PCM_16BIT, minOutBufferSizeRound,
                AudioTrack.MODE_STREAM
            )*/
            val audioTrack = AudioTrack.Builder()
                .setTransferMode(AudioTrack.MODE_STREAM)
                .setAudioFormat(
                    AudioFormat.Builder()
                        .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                        .setSampleRate(sampleRate)
                        .setChannelMask(AudioFormat.CHANNEL_OUT_MONO)
                        .build()
                )
                //.setBufferSizeInBytes(audioOutBufferSize)
                .setBufferSizeInBytes(audioMinOutBufferSize)
                .setPerformanceMode(AudioTrack.PERFORMANCE_MODE_LOW_LATENCY)
                .build()
            log("play native bufferCapacityInBytes ${audioTrack.bufferCapacityInFrames * 2}")
            log("play native bufferSizeInBytes ${audioTrack.bufferSizeInFrames * 2}")

            val size = remoteRecFrame
            val silence = ByteArray(remoteRecFrame)

            // receiveBufferQueueSemaphore.acquire()  // wait for first packet to be ready to play
            // receiveBufferQueueSemaphore.acquire()  // wait for first packet to be ready to play
            audioTrack.play()
            //receiveBufferQueueSemaphore.release()

            // hard-coded constants,
            // ok for high-quality fast network connection
            // and more or less stable capturing intervals
            // like 0 0 0 21.2 instead of 5.33 5.33 5.33 5.33
            val minPlayQueue = 2
            var minPlayQueue2 = 3
            var maxPlayQueue = 10
            assert(minPlayQueue >= 2)
            assert(minPlayQueue2 > minPlayQueue)
            // there should be minimal gap between min-play-queue-2 and max-play-queue
            assert(maxPlayQueue - minPlayQueue >= 2)

            var i = 0
            var pos = 0
            var totalUnderruns = 0
            while (true) {
                
                receiveBufferQueueSemaphore.acquire() // enquue audio asap
                i++
                
                val playQueueLen = receiveBufferQueueSemaphore.availablePermits() + 1
                
                if (i and 0x7f == 0) {
                    minPlayQueue2 = (avgRecvBurstSize + 1).coerceIn(minPlayQueue + 1, receiveBufferFragments - 1)
                    maxPlayQueue = (avgRecvBurstSize * 3 / 2 + 1).coerceIn(minPlayQueue2 + 2, receiveBufferFragments - 1)
                    log("minPlayQueue2 $minPlayQueue2  maxPlayQueue $maxPlayQueue  queue len $playQueueLen")
                }
                
                
                if (playQueueLen <= minPlayQueue) {
                    log("delay playback because the queue is almost empty (${playQueueLen} packets left)")
                    //pos = (pos + size) % receiveBuffer.size
                    receiveBufferQueueSemaphore.release()
                    
                    // val frameTime = (size * 1000 / 2 / playChannels / sampleRate).toLong()
                    // val sleep = (frameTime / 2).coerceIn(1, frameTime)
    
                    receiveBufferQueueSemaphore.acquire(minPlayQueue2)
                    receiveBufferQueueSemaphore.release(minPlayQueue2)
                    
                    // Thread.sleep((sleep / 2).toLong())
                    // Thread.sleep(1)
                    continue
                }
                
                if (playQueueLen >= maxPlayQueue) {
                    log("skip one play packet because the queue is too big - queue len: " + playQueueLen)
                    pos = (pos + size) % receiveBuffer.size
                    continue
                }
                
                audioTrack.write(receiveBuffer, pos, size) // blocking
                pos = (pos + size) % receiveBuffer.size

                // log("delay playback because queue is too small")
                // receiveBufferAccessSmaphore.release()
                // return the last packet to "not played" state
                // receiveBufferQueueSemaphore.release()
                // wait a little
                // val time = System.currentTimeMillis()
                // audioTrack.write(silence, 0, size)
                // val writeTime = System.currentTimeMillis() - time
                // val sleep = 1 * size / 2 / playChannels * 1000 / sampleRate
                // val effSleep = sleep - writeTime
                // if (effSleep > 0) {
                //     Thread.sleep((effSleep).toLong())
                //     log("delay playback because queue is too small. sleep for " + effSleep + " ms")
                // } else {
                //     log("delay playback because queue is too small")
                // }
                // audioTrack.write(silence, 0, size)
                /*if (i and 0x7f == 0) {
                    val totalUnderrunsNow = audioTrack.underrunCount
                    val underruns = totalUnderrunsNow - totalUnderruns
                    totalUnderruns = totalUnderrunsNow
                    //log("play queue packets $playQueueLen  (${playQueueLen * remoteRecFrame * 1000 / 2 / playChannels / sampleRate} ms)   overruns $underruns")
                    log("underruns $underruns")
                    if (underruns > 0) {
                        maxPlayQueue++
                        log("increase max play queue to $maxPlayQueue")
                    } else if (underruns == 0) {
                        if (maxPlayQueue > minPlayQueue + 2) {
                            maxPlayQueue--
                            log("decrease max play queue to $maxPlayQueue")
                        }
                    }
                }*/
                // if (i and 0xff == 0) {
                //     log("rb read pos $receiveBufferReadPos write pos $receiveBufferWritePos playback underrun " + audioTrack.underrunCount)
                // }
            }
        } catch (e: Exception) {
            log("play theread error: " + e.message.toString())
        }
    }
}

fun writeAudio(buffer: ByteArray, offset: Int, size: Int) {
    assert(size == remoteRecFrame)
    // receiveBufferAccessSmaphore.acquire()
    
    // if (receiveBufferWritten - receiveBufferRead < size * 8) {
    val playQueueLen = receiveBufferQueueSemaphore.availablePermits()
    
    if (playQueueLen < receiveBufferFragments - 2) {
    // if (true) {
        buffer.copyInto(receiveBuffer, receiveBufferWritePos, offset, offset + size)
        receiveBufferWritePos = (receiveBufferWritePos + size) % receiveBuffer.size
        // receiveBufferWritePos += size
        // if (receiveBufferWritePos == receiveBuffer.size) {
        //     receiveBufferWritePos = 0
        // }
        //log("write audio: wp = " + receiveBufferWritePos / remoteRecFrame)
        // receiveBufferWritten += size
    
        // receiveBufferAccessSmaphore.release()
    
        receiveBufferQueueSemaphore.release() // the packet is ready
    
    
    } else {
        // if there are too much of queued packet - drop the current packet
        // receiveBufferAccessSmaphore.release()
        log("drop incoming packet because queue is full - " + playQueueLen + " packets")
    }
}

fun createReceiveBuffer(remoteRecFrame: Int, audioMinInBufferSize: Int): ByteArray {
    // the bare minimum for receive buffer size is 3 fragments:
    // play-part + read-part + write-part
    // but because of bursts we need to be able to
    // save one full packets burst and be able
    // to start receiving the next one at the same time
    // so we alloc large receive buffer
    // and deal with too long queues elsewhere
    // after analysis of burst lengths
    var receiveBufferSize = remoteRecFrame * 32
    // guarantee at least multiply of native buffer size
    while (receiveBufferSize < audioMinInBufferSize) {
        receiveBufferSize += remoteRecFrame
        log("info: receiveBuffer size increased from ${receiveBufferSize - remoteRecFrame} to $receiveBufferSize")
    }
    log("receiveBuffer final size: $receiveBufferSize bytes")
    return ByteArray(receiveBufferSize)
}

@Suppress("DEPRECATION")
class ReceiveThread : Runnable {
    override fun run() {
        log("receive thread run")
        setThreadPolicy()
    
        val frameTimeFloat = (remoteRecFrame * 1000.0 / 2 / playChannels / sampleRate)
        
        receiveBuffer = createReceiveBuffer(remoteRecFrame, audioMinInBufferSize)
        receiveBufferFragments = receiveBuffer.size / remoteRecFrame
        log("receiveBufferFragments $receiveBufferFragments")
        assert(receiveBufferFragments >= 3)
        // min triple buffering: play-part, read-part, write-part
        assert(receiveBuffer.size >= audioMinInBufferSize)
        
        val maxRecPacketSize = 32768
        val buffer = ByteArray(maxRecPacketSize)
    
        val packet = DatagramPacket(buffer, buffer.size)
        //val silence = ByteArray(remoteRecPacketSize - 8)
        //val buffer = ByteArray(2836)
    
        val loopbackAddress = InetAddress.getLoopbackAddress()
    
        var lastSeq: ULong = 0UL;
        //var prevUnderrun = 0
    
        var prevReceiveTime = System.currentTimeMillis()
        var receiveTimeDeltaSum = 0L
        var receiveN = 0L
        
        var burstSize = 0
        var burstTime = 0
        
        var burstSizeSum = 0L
        var burstSizeN = 0L
    
        // receive incoming udp packets as fast as possible
        while (true) {
            try {
                socket.receive(packet)
    
                val now = System.currentTimeMillis()
    
                val n = packet.length
                //log("packet received size " + n)
                //if (n == remoteRecPacketSize) {
                if (n > 4) { // normal packet
                    //assert(n - 8 == remoteRecFrame)
    
                    val delta = now - prevReceiveTime
                    receiveTimeDeltaSum += delta
                    receiveN++
                    if (receiveN > 121) {
                        receiveTimeDeltaSum /= 2;
                        receiveN /= 2;
                    }
                    
                    burstSize++;
                    burstTime += delta.toInt()
                    var idealTime = frameTimeFloat * burstSize
                    // log("delta $delta   burstTime $burstTime <=> ${idealTime}   ")
                    // minus averageError
                    if (burstTime >= idealTime - burstSize * .5) {
                        val minPlayQueue = burstSize + 1
                        // log("recvBurstSize $burstSize")
    
                        burstSizeSum += burstSize
                        burstSizeN++
                        
                        if (burstSizeN > 71) {
                            avgRecvBurstSize = (burstSizeSum / burstSizeN).toInt()
                            log("avg recvBurstSize ${burstSizeSum / burstSizeN}")
                            burstSizeSum /= 2
                            burstSizeN /= 2
                        }
                        
                        // burstTime = burstTime - ceil(idealTime).toInt()
                        // if (burstTime < 0) burstTime = 0
                        burstTime = 0
                        burstSize = 0
                        
                    }
    
                    val playQueueLen = receiveBufferQueueSemaphore.availablePermits()
                    // log(
                    //     "recv delta ${delta.toString().padStart(3)} ms, avg ${receiveTimeDeltaSum / receiveN} ms" +
                    //     "    " +
                    //     "play queue packets $playQueueLen  (${playQueueLen * remoteRecFrame * 1000 / 2 / playChannels / sampleRate} ms)"
                    // )
    
                    remoteAddress = packet.address
                    //if (!hasKnownClient)
                    //audioTrack.write(silence, 0, silence.size)
                    hasKnownClient = true
                    //audioTrack.write(buffer, 0, buffer.size, AudioTrack.WRITE_NON_BLOCKING)
                    val seq = byteArrayToULong(buffer)
                    if (seq == lastSeq + 1UL) {
                        //val written = audioTrack.write(buffer, 8, buffer.size - 8, AudioTrack.WRITE_NON_BLOCKING)
                        //val written = audioTrack.write(buffer, 8, buffer.size - 8)
                        writeAudio(buffer, 8, n - 8);
                        //if (written != 1024) log("written " + written)
                        lastSeq = seq
                    } else if (seq > lastSeq + 1UL) {
                        //Log.i("debug", "packet received " + buffer.joinToString())
                        //Log.d("debug", "playback underrun " + audioTrack.underrunCount);
                        // write silence and new data
                        /*var limit = 1
                        while (seq != lastSeq + 1UL && limit >= 0) {
                            audioTrack.write(silence, 0, silence.size)
                            lastSeq++
                            limit--
                        }*/
                        //if (seq - lastSeq > 2UL)
                        //audioTrack.write(silence, 0, silence.size)
                        /*val underrun = audioTrack.underrunCount
                        val delta = underrun - prevUnderrun
                        for (i in 1..delta)*/
                        //audioTrack.write(buffer, 8, buffer.size - 8)
                        writeAudio(buffer, 8, n - 8)
                        if (seq == 0UL) {
                            log("incoming stream restart")
                        } else {
                            log("detected incoming packet loss " + seq + " " + lastSeq + "  (" + (seq - lastSeq - 1UL) + ")")
                        }
                        lastSeq = seq
                    } else if (seq < lastSeq) { // packet loss or restart
                        writeAudio(buffer, 8, n - 8)
                        if (seq == 0UL) {
                            log("incoming stream restart")
                        } else {
                            log("wrong incoming packet sequence number " + seq + " " + lastSeq)
                            //Log.d("debug", "playback underrun " + audioTrack.underrunCount);
                        }
                        lastSeq = seq
                        // do nothing
                    } else if (seq == lastSeq) {
                        log("detected duplicate incoming packet " + seq + " " + lastSeq)
                        //Log.d("debug", "playback underrun " + audioTrack.underrunCount);
                        // do nothing
                    }
                } else if (
                    n == 4 &&
                    buffer[0] != appId[0] &&
                    buffer[1] != appId[1] &&
                    buffer[2] != appId[2] &&
                    buffer[3] != appId[3]
                ) {
                    // looks like broadcast packet
                    log("broadcast packet received from " + packet.address)
                    remoteAddress = packet.address
                    hasKnownClient = true
                } else {
                    if (
                        buffer[0] == appId[0] &&
                        buffer[1] == appId[1] &&
                        buffer[2] == appId[2] &&
                        buffer[3] == appId[3]
                    ) {
                        log("ignore self-broadcast packet")
                    } else if (packet.address == loopbackAddress) {
                        log("loopback packet ignored")
                    } else {
                        log("unrecognized packet received, size " + n);
                    }
                    hasKnownClient = false
                }
    
                prevReceiveTime = now
    
            } catch (e: Exception) {
                log("receive-thread in-loop errors: " + e.message.toString())
                Thread.sleep(500)
                //log("playback underrun " + audioTrack.underrunCount);
                hasKnownClient = false
                remoteAddress = null
            }
        }
    }
}

class RecThread(val context: Context) : Runnable {
    @SuppressLint("MissingPermission")
    override fun run() {
        log("rec thread run")
        setThreadPolicy()
        try {
            assert(recChannels == 1) // limitation of the current implementation

            val audioRecord = AudioRecord(
                audioSource,
                sampleRate,
                AudioFormat.CHANNEL_IN_MONO,
                AudioFormat.ENCODING_PCM_16BIT,
                audioMinInBufferSize
            )
    
            //val frameSize = 256
            val nativeBufferSize = audioRecord.bufferSizeInFrames * 2
            log("audioRecord nativeBufferSize ${nativeBufferSize}")
    
            //val buffer = ByteArray(audioInBufferSize / 2) // read part of the buffer
            val frameSize = (nativeBufferSize * 2 / 3) / 2 // xiaomi redmi 6a android8.1
    //            val frameSize = audioInBufferSize / 2
            //socket.soTimeout = 2 * frameSize * 1000 / recChannels / sampleRate // ms (receive only)
            log("rec frame-size bytes " + frameSize)
            val frameSamples = frameSize / 2
            //val frameSamples = 1440 //audioRecord.bufferSizeInFrames / 2
            val buffer = ByteArray(8 + frameSize) // read part of the buffer
            var packet: DatagramPacket? = null
    
            // special packet for broadcasting
            val bcastBuffer = ByteArray(4)
            bcastBuffer[0] = appId[0]
            bcastBuffer[1] = appId[1]
            bcastBuffer[2] = appId[2]
            bcastBuffer[3] = appId[3]
    
    
            var prevReadTime = System.currentTimeMillis()
            var seq: UByte = 0u
            
            val broadcastAddress = getBroadcastAddress(context)
            val bcastPacket = DatagramPacket(
                bcastBuffer,
                bcastBuffer.size,
                //InetAddress.getByName("255.255.255.255"),
                //InetAddress.getByName("192.168.43.255"),
                broadcastAddress,
                port
            )
            
            audioRecord.startRecording()
    
            while (true) {
                try {
                    val read = audioRecord.read(buffer, 8, frameSize)
                    //val read = audioRecord.read(buffer, 0, frameSize, AudioRecord.READ_NON_BLOCKING)
    
                    ///*if (read > 0) {
                        val now = System.currentTimeMillis()
                        val delta = now - prevReadTime
                        //log("read " + read + " bytes, delta " + delta)
                        prevReadTime = now
    //                        val frameTime = frameSamples * 1000 / 48000;
    //                        if (delta < frameTime - 5) {
    //                            Thread.sleep(frameTime - 5 - delta)
    //                        }
                    //}*/
    //                    if (delta < frameSamples * 1000 / 48000 / 2) {
    //                        log("sleep for " + (frameSamples * 1000 / 48000 / 2))
    //                        Thread.sleep((frameSamples * 1000 / 48000 / 2).toLong())
    //                    }
    
                    //log("audio recorded $read")
                    if (read > 0) {
                        //Log.i("debug", "send packet " + buffer.joinToString())
    
                        if (hasKnownClient && remoteAddress != null) {
                            if (packet != null && packet.address == remoteAddress) {
                                // known peer
                            } else {
                                log("new peer remote ip")
                                packet = DatagramPacket(
                                    buffer, buffer.size, remoteAddress, port
                                )
                            }
                            seq++
                            buffer[0] = seq.toByte()
                            socket.send(packet)
                        } else {
                            // get fresh broadcast address because
                            // the network could change
                            val broadcastAddress = getBroadcastAddress(context)
                            log("no remote address - bcast to $broadcastAddress")
                            //log("broadcastAddress $broadcastAddress")
                            bcastPacket.address = broadcastAddress
                            socket.send(bcastPacket)
                            Thread.sleep(237) // limit rate
                        }
    
                    } else if (read == 0) {
                        // nop
                    } else {
                        throw Error("audio captured bytes is less than buffer size")
                    }
    
                } catch (e: Exception) {
                    log("rec-thread in-loop error: " + e.message.toString())
                    // there should be no critical errors
                    // excluding mb send errors
                    Thread.sleep(100) // just in case
                }
    
            }
        } catch (e: Exception) {
            log("rec-threadd out of loop error: " + e.message.toString())
    //            System.err.println(e)
    //            e.printStackTrace()
        }
    }
}

class MainActivity : AppCompatActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)
    
        log("activity created successfully")
        //setThreadPolicy()
    
        val result = ContextCompat.checkSelfPermission(applicationContext, RECORD_AUDIO)
        if (result != PackageManager.PERMISSION_GRANTED) {
            ActivityCompat.requestPermissions(
                this@MainActivity,
                arrayOf(RECORD_AUDIO),
                1
            )
        }
        val result2 = ContextCompat.checkSelfPermission(applicationContext, RECORD_AUDIO)
        if (result2 != PackageManager.PERMISSION_GRANTED) {
            throw Error("No permission to record audio")
        }
    
        val nativeSampleRate = AudioTrack.getNativeOutputSampleRate(AudioTrack.MODE_STREAM)
        log("native out sample rate: " + nativeSampleRate)
        if (sampleRate != nativeSampleRate)
            log("sample rate does not match native sample rate")
    
        audioMinOutBufferSize = AudioTrack.getMinBufferSize(
            sampleRate,
            AudioFormat.CHANNEL_OUT_MONO,
            AudioFormat.ENCODING_PCM_16BIT
        )
        log("audioOutBufferSize $audioMinOutBufferSize")
        //audioOutBufferSize = powerOfTwo(audioMinOutBufferSize)
        //log("audioOutBufferSize $audioOutBufferSize (round)")
    
        audioMinInBufferSize = AudioRecord.getMinBufferSize(
            sampleRate,
            AudioFormat.CHANNEL_IN_MONO,
            AudioFormat.ENCODING_PCM_16BIT)
        log("audioInBufferSize $audioMinInBufferSize")
        //audioInBufferSize = powerOfTwo(audioMinInBufferSize)
        //log("audioInBufferSize $audioOutBufferSize (round)")
    
        appId = byteArrayOf(
            (0..Byte.MAX_VALUE).random().toByte(),
            (0..Byte.MAX_VALUE).random().toByte(),
            (0..Byte.MAX_VALUE).random().toByte(),
            (0..Byte.MAX_VALUE).random().toByte()
        )
    
        socket.setBroadcast(true);
        // try to avoid too large outgoing buffering
        socket.sendBufferSize = audioMinInBufferSize
        // set receive buffer to a rather large value because
        // packets can go in bursts from the peer
        // note: queue overflow control is done elsewere
        socket.receiveBufferSize = remoteRecPacketSize * 32
    
        val playThread = Thread(PlayThread())
        playThread.start()
    
        val receiveThread = Thread(ReceiveThread())
        receiveThread.start()
    
        val recThread = Thread(RecThread(this))
        recThread.start()
    
        /*val button1 = this.findViewById<Button>(R.id.button)
        button1.setOnClickListener(
            fun(view: View?) {
    
            }
        )*/
    
    }
    
    
    
    // acquire wi-fi lock to be able to receive broadcasts
    private var wm: WifiManager? = null
    private var multicastLock: MulticastLock? = null
    
    override fun onResume() {
        super.onResume()
        if (wm == null) wm = getSystemService(WIFI_SERVICE) as WifiManager
        if (wm != null) {
            if (multicastLock == null) {
                multicastLock = wm!!.createMulticastLock("stackoverflow for the win")
                if (multicastLock != null) {
                    multicastLock!!.setReferenceCounted(true)
                } else {
                    log("can not get wi-fi lock")
                }
            }
            if (multicastLock != null && !multicastLock!!.isHeld) multicastLock!!.acquire()
        } else {
            log("can not get wi-fi manager")
        }
    }
    
    override fun onPause() {
        super.onPause()
        if (multicastLock != null && multicastLock!!.isHeld) multicastLock!!.release()
    }
    }
    
    fun getDhcpBroadcastAddress(context: Context): InetAddress? {
    try {
        val wifi: WifiManager = context.getSystemService(WIFI_SERVICE) as WifiManager
        val dhcp = wifi.dhcpInfo
        if (dhcp != null) {
            val broadcast = dhcp.ipAddress and dhcp.netmask or dhcp.netmask.inv()
            val quads = ByteArray(4)
            for (k in 0..3) quads[k] = (broadcast shr k * 8).toByte()
            return InetAddress.getByAddress(quads)
        }
    } catch (e: Exception) {
        log(e.message.toString())
    }
    return null
    }
    
    fun getBroadcastAddress(context: Context): InetAddress {
    System.setProperty("java.net.preferIPv4Stack", "true")
    var dhcpAddress = getDhcpBroadcastAddress(context)
    if (dhcpAddress != null && dhcpAddress.toString() != "/255.255.255.255") {
        return dhcpAddress
    } else {
        // hard-coded wi-fi hot spot broadcast address
        return InetAddress.getByName("192.168.43.255")
        /*try {
            val inet = InetAddress.getLocalHost()
            val net: NetworkInterface = NetworkInterface.getByInetAddress(inet)
            val interfaceAddresses = net.interfaceAddresses
            if (interfaceAddresses.size > 0) {
                return interfaceAddresses[0].broadcast
            }
        } catch (e: Exception) {
            log(e.message.toString())
        }
        return InetAddress.getByName("255.255.255.255")*/
    }
}