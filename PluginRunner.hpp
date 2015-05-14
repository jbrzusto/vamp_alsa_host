#ifndef PLUGINRUNNER_HPP
#define PLUGINRUNNER_HPP

#include <vamp-hostsdk/PluginHostAdapter.h>
#include <vamp-hostsdk/PluginInputDomainAdapter.h>
#include <vamp-hostsdk/PluginBufferingAdapter.h>
#include <vamp-hostsdk/PluginLoader.h>
#include <vamp-sdk/Plugin.h>
#include <vamp-sdk/PluginBase.h>
#include <stdint.h>
#include <set>
#include <memory>
#include <fftw3.h>

using namespace Vamp;
using namespace Vamp::HostExt;

#include "ParamSet.hpp"
#include "Pollable.hpp"

typedef std::map < string, weak_ptr < Pollable > > OutputListenerSet;

class AlsaMinder;

class PluginRunner : public Pollable {
public:
  string             label;            // name of this plugin runner (used in commands)
  string             devLabel;         // label of device from which plugin receives input
  string             pluginSOName;     // name of shared object where plugin resides
  string             pluginID;         // id of plugin
  string             pluginOutput;     // name of output to obtain from plugin
  ParamSet           pluginParams;     // parameter settings for plugin
  static const int   MAX_NUM_CHAN = 16;// maximum number of channels a plugin can handle
protected:
  static PluginLoader *pluginLoader;   // plugin loader (singleton)
  VampAlsaHost *     host;             // host
  int                rate;             // sampling rate for plugin; frames per second
  int                hwRate;           // sampling rate of hardware to which we're attached
  unsigned int       numChan;          // number of channels plugin uses
  long long          totalFrames;      // total number of (decimated) frames this plugin instance has processed
  long long          totalFeatures;    // total number of "features" (e.g. lotek pulses) seen on this FCD
  Plugin *           plugin;           // VAMP plugin we'll be running on this fcd
  float **           plugbuf;          // pointer to one buffer for each channel (left, right) of float data for plugin
  float *            winBuf;           // pointer to buffer of windowing coefficients for requested block size, when supply FreqDomain data
  int                outputNo;         // index of plugin output corresponding to pluginOutput
  int                blockSize;        // size (in frames) of blocks sent to plugin
  int                stepSize;         // amount (in frames) by which consecutive blocks differ
  int                framesInPlugBuf;  // number of frames in plugin buffers since last call to plugin->process()
  bool               isOutputBinary;   // if true, output from plugin is not text.  For text outputs, if
  int                resampleDecim;    // the number of incoming hardware frames to combine into a frame for the plugin
  float              resampleScale;    // scale factor for a sum of hardware samples
  int                resampleCountdown; // number of hardware frames left to get before we have a resampled frame
  int *              partialFrameSum;  // partial frame sums from previous call to handleData
  double             lastFrametimestamp; // frame timestamp from prvious call to handleData
  bool               freqDomain;       // true when plugin receives data in frequency domain
  int                channelOutputCount; // count calls to queueOutput; only dispatch to plugin when all channels ready.
  // the output buffer gets filled before it can be written to a socket,
  // the oldest output is discarded line by line, so that any output line
  // is either completely written or not written at all.  For binary output,
  // we just discard at arbitrary boundaries.

  OutputListenerSet     outputListeners;     // connections receiving output from this plugin, if any.

public:
  PluginRunner(const string &label, const string &devLabel, int rate, int hwRate, int numChan, const string &pluginSOName, const string &pluginID, const string &pluginOutput, const ParamSet &ps);
  ~PluginRunner();

  //  void handleData(long avail, int16_t *src0, int16_t *src1, int step, double frameTimestamp);
  bool queueOutput(const char *p, uint32_t len, double timestamp);

  /* this class does not receive or send data via file descriptors, so
     the following methods are no-ops (and really, this shouldn't
     descend from Pollable, but that's our base class.  FIXME:
     refactor class hierarchy so queueOutput() is inherited from a
     different parent than the fd-polling-related methods */

  int getNumPollFDs() {return 0;}
  int getPollFDs (struct pollfd * pollfds) {return 0;}
  int getOutputFD(){return 0;}; 
  void handleEvents (struct pollfd *pollfds, bool timedOut, double timeNow) {};

  /* new methods */

  bool addOutputListener(string connLabel);
  void removeOutputListener(string connLabel);
  void removeAllOutputListeners();
  int loadPlugin();
  void outputFeatures(Plugin::FeatureSet features, string prefix);
  string toJSON();
  void stop(double timeNow);
  int start(double timeNow);
  void setParameters(ParamSet &ps);

  /* accessors */

  bool getFreqDomain() { return freqDomain;};
  int getBlockSize() { return blockSize;};
  int getStepSize() { return stepSize;};
  float ** getPlugBuf() { return plugbuf;};

private:
  void delete_privates();
  
};


#include "AlsaMinder.hpp"

#endif // PLUGINRUNNER_HPP
