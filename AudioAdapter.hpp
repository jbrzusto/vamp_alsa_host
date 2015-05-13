#ifndef AUDIOADAPTER_HPP
#define AUDIOADAPTER_HPP

#include <boost/circular_buffer.hpp>
#include <fftw3.h>
#include <cmath>
#include "Pollable.hpp"
#include "WavFileHeader.hpp"
class AlsaMinder;
//#include "AlsaMinder.hpp"

// internal class to mediate between audio producer (AlsaMinder)
// and consumer (PluginRunner, TCPConnection)
//
// responsible for buffering, downsampling, channel selection, 
// conversion to spectrum, FM demodulation
//
// Each audio producer has a set of AudioAdapter consumers, which it owns.
//
// Output is sent via the listener's queueOutput function, which
// informs the listener of available data.  Some listeners,
// e.g. PluginRunner, provide their own buffers, and data are
// stored there, in which case the call to queueOutput does not
// cause any copying of data.

class AudioAdapter {
public: 
  typedef int16_t sample_t;
  typedef boost::circular_buffer < sample_t > circBuf;
  static const int  MAX_CHANNELS          = 2;      // maximum of two channels per device
  static const int PERIOD_FRAMES          = 9600;   // must match value in AlsaMinder.hpp

  // types of output desired by 
typedef enum {
OT_INT,      // raw audio, possibly down-sampled
  OT_FLOAT,    // audio as floats, separated by channel, possibly down-sampled
  OT_SPECTRUM, // channel-wise spectrum, possibly down-sampled first; blockSize and stepSize determine
// fft window size and overlap.
  OT_FM        // FM-demodulated audio, possibly down-sampled first
  } OutputType;

  // constructor
  //!<  rate: sampling rate, Hz
  //!<  numChan: number of channels
  //!<  ot: output type for this adapter
  //!<  blockSize: number of frames to send to listener on a call; 0 means send data as received in whatever size is available
  //!< stepSize: indicates the number of frames by which each call to listener's queueOutput function is advanced.  So if 
  //   stepSize < blockSize, consecutive calls have overlapping data.  So far, only use with spectrum data.
  //!< listenerLabel string identifying Pollable object whose queueOutput method is called with new data
  //!< bufs: pointers to buffers for float output from each channel; 
  //!< writeWavFileHeader: if true, write a .WAV file header before any other output is sent to the listener
  //!< numFrames: if non-zero, output to listener ends after numFrames frames have been sent.  Also used when writing
  // a .WAV file header.

  AudioAdapter (int rate, int hwRate, int numChan, int maxFrames, OutputType ot, int blockSize, int stepSize, string &listenerLabel, float ** buffs = 0, bool writeWavFileHeader = false, int numFrames=0 );

  ~AudioAdapter();

  // handle data from AlsaMinder.
  //!< a1: array_range giving first segment of consecutive interleaved samples
  //!< a2: array_range giving second segment of consecutive interleaved samples
  // each array contains a multiple of numChan interleaved samples (i.e. frames are not
  // split between array segments)
  //!< frameTimestamp: timestamp of first frame in a1.
  // If blockSize > 0, this method is only called when there are at least
  // blockSize * numChan samples available in the combined array ranges.
  // If blockSize == 0, this method is called whenever data arrive.
  // The function returns the number of samples which can be discarded.

int handleData(circBuf::array_range a1, circBuf::array_range a2, double frameTimestamp);

circBuf * getCircularBuffer() {return & cb;};
int getBlockSize() { return blockSize;};
int getNumChan() { return numChan;};
bool setOutputType (OutputType ot);

protected:

  // structural members:
  int        rate;      //!< target rate for this output
  int        hwRate;    //!< hardware sampling rate 
  int        numChan;   //!< number of channels on input
  int        maxFrames; //!< maximum number of frames supplied to handleData()
  OutputType ot;
  int        blockSize;
  int        stepSize;
  int        numFrames;
  string     listenerLabel;
  float **   buffs;
  bool       weOwnBuffs;
  bool       writeWavFileHeader;

  // implementation members:
  circBuf cb; //!< circular buffer of raw interleaved samples

  int numOutChan; //!< equals numChan except that if output is FM it is 1.

  int   downSampleCount;  //!< count of how many samples we've accumulated since last down sample
  int   downSampleAccum[MAX_CHANNELS];  //!< accumulator for downsampling

  int16_t * downSampleBuf; // !< if downsampling and integer output is needed, this is the buffer of interleaved, downsampled data

  float *winBuf; //!< hold windowing coefficients if output type is SPECTRUM
  float * fftInput; //!< hold input data for FFT; if numChan is 1, this is really fftwf_complex (i.e. interleaved)
  fftwf_plan fftPlan; //!< hold FFTW plan, if needed

  sample_t           downSampleFactor; // by what factor do we downsample input audio for raw listeners

  // internal member functions
  float * hammingWindow(int N); //!< generate a Hamming window of size N
};

#endif // AUDIOADAPTER_HPP
