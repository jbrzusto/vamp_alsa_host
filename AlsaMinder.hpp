#ifndef ALSAMINDER_HPP
#define ALSAMINDER_HPP

#include <string>
#include <stdexcept>
#include <sstream>
#include <iomanip>
#include <memory>
#include <cmath>

using namespace std;

#include <boost/circular_buffer.hpp>
#include <alsa/asoundlib.h>

#include "Pollable.hpp"
#include "PluginRunner.hpp"
#include "WavFileHeader.hpp"
#include "AudioAdapter.hpp"
//class AudioAdapter;

typedef int16_t sample_t;
typedef boost::circular_buffer < sample_t > circBuf;
typedef std::map < string, AudioAdapter * > ListenerSet;

class AlsaMinder : public Pollable {
public:


  static const int  PERIOD_FRAMES         = 9600;   // 20 periods per second @ 192kHz, 10 @ 96 kHz, 5 @ 48 kHz
  static const int  BUFFER_FRAMES         = 131072; // 128K appears to be max buffer size in frames
  static const int  MAX_AUDIO_QUIET_TIME  = 10;     // 10 second maximum quiet time before we decide an audio data stream is dry and try restart it
  static const int  MAX_CHANNELS          = 2;      // maximum of two channels per device
    
  string             alsaDev;          // ALSA path to audio device (e.g. hw:CARD=V10)
  int                rate;             // sampling rate to supply plugins with
  unsigned int       hwRate;           // sampling rate of hardware device
  unsigned int       numChan;          // number of channels to read from device

protected:

  ListenerSet       listeners;        // set of listeners.  For each, we maintain a circular buffer of size 2 * PERIOD_FRAMES frames;

  snd_pcm_t *       pcm;              // handle to open pcm device
  snd_pcm_uframes_t buffer_frames;    // buffer size given to us by ALSA (we attempt to specify
                                      // it)
  snd_pcm_uframes_t period_frames;    // period size given to us by ALSA (we attempt to specify
                                      // it)
  unsigned short    revents;          // demangled version of revent returned after poll()
  long long         totalFrames;      // total frames seen on this device since start of capture
  double            startTimestamp;   // timestamp device was (most recently) started (-1 if
                                      // never)
  double            stopTimestamp;    // timestamp device was (most recently) stopped or opened
                                      // (-1 if not opened yet)
  double            lastDataReceived; // time at which data was last received (or at which the
                                      // device was most recently started); used to detect
                                      // random audio stop (e.g. due to hub device reset)
                                      // -1 if never started 
  bool              shouldBeRunning;  // should this device be running?
  bool              stopped;          // is this device stopped?  (by which we mean not
                                      // streaming USB audio)
  int               hasError;         // if non-zero, the most recent error this device got
                                      // while we polled it? (this would have stopped it)
  int               numFD;            // number of file descriptors required for polling on this
                                      // device

public:

  int open();
  void addListener (string & label, AudioAdapter *ad); // label is the Pollable label of the consumer
  void removeListener (string & label);

  AlsaMinder(const string &alsaDev, int rate, unsigned int numChan, const string &label, double now);

  ~AlsaMinder();

  string about();

  string toJSON();

  virtual int getNumPollFDs ();

  virtual int getPollFDs (struct pollfd *pollfds);

  int getOutputFD(){return 0;}; 

  virtual void handleEvents ( struct pollfd *pollfds, bool timedOut, double timeNow);
  int start(double timeNow);
  void stop(double timeNow);
  
  int getNumListeners() { return listeners.size();};
  AudioAdapter * getFirstListener() { return listeners.begin()->second; };
  
protected:
  
  void delete_privates();
  int do_start(double timeNow);
  void do_stop(double timeNow);

};

#endif // ALSAMINDER_HPP
