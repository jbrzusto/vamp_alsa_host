#include "PluginRunner.hpp"

void PluginRunner::delete_privates() {
  /*
  if (plugin) {
    delete plugin;
    plugin = 0;
  }
  */
  if (plugbuf) {
    for (unsigned int i=0; i < numChan; ++i) {
      if (plugbuf[i])
        fftwf_free (plugbuf[i]);
    }
    delete [] plugbuf;
    plugbuf = 0;
  }
  if (partialFrameSum) {
    delete [] partialFrameSum;
    partialFrameSum = 0;
  }
  if (winBuf) {
    fftwf_free (winBuf);
    winBuf = 0;
  }
};

int PluginRunner::loadPlugin() {
  // load the plugin, make sure it is compatible and that all parameters are okay.

  // get an instance of the plugin loader, 

  if (! pluginLoader)
    pluginLoader =  PluginLoader::getInstance();

  PluginLoader::PluginKey key = pluginLoader->composePluginKey(pluginSOName, pluginID);

  plugin = pluginLoader->loadPlugin (key, rate, 0); // no adapting, rather than PluginLoader::ADAPT_ALL_SAFE;

  if (! plugin) {
    return 1;
  }

  // make sure the plugin is compatible: it must run in the time domain and accept an appropriate number of channels
        
  if (plugin->getInputDomain() != Plugin::TimeDomain
      || plugin->getMinChannelCount() > numChan
      || plugin->getMaxChannelCount() < numChan) {
    return 2;
  }

  // get preferred block and step sizes, and do sanity check

  blockSize = plugin->getPreferredBlockSize();
  stepSize = plugin->getPreferredStepSize();
        
  if (blockSize == 0) {
    blockSize = 1024;
  }
  if (stepSize == 0) {
    stepSize = blockSize;
  } else if (stepSize > blockSize) {
    blockSize = stepSize;
  }

  // allocate buffers to transfer float audio data to plugin

  plugbuf = new float*[numChan];
  for (unsigned c = 0; c < numChan; ++c)
    // use fftwf_alloc_real to make sure we have alignment suitable for in-place SIMD FFTs 
    plugbuf[c] =  fftwf_alloc_real(blockSize + 2);  // FIXME: is "+2" only to leave room for DFT?; 

  // if requesting frequency domain, allocate a windowing coefficient buffer and fill it
  if (plugin->getInputDomain() == Vamp::Plugin::FrequencyDomain) {
    freqDomain = true;
    winBuf = hammingWindow(blockSize);
  }
    
  // make sure the named output is valid
        
  Plugin::OutputList outputs = plugin->getOutputDescriptors();
        
  for (size_t i = 0; i < outputs.size(); ++i) {
    if (outputs[i].identifier == pluginOutput) {
      outputNo = i;
      break;
    }
  }

  if (outputNo < 0) {
    return 3;
  }

  // set the plugin's parameters

  setParameters(pluginParams);

  // initialise the plugin

  if (! plugin->initialise(numChan, stepSize, blockSize)) {
    return 4;
  }

  // Try set a plugin parameter called "__batch_host__" to 1.
  // This allows a plugin to e.g. produce different output depending
  // on whether it is run with vamp-alsa-host or audacity
  // (e.g. gap size labels when run with audacity, not with vamp-alsa-host)

  plugin->setParameter("__batch_host__", 1);

  return 0;
};

PluginRunner::PluginRunner(const string &label, const string &devLabel, int rate, int hwRate, int numChan, const string &pluginSOName, const string &pluginID, const string &pluginOutput, const ParamSet &ps):
  Pollable (label),
  label(label),
  devLabel(devLabel),
  pluginSOName(pluginSOName),
  pluginID(pluginID),
  pluginOutput(pluginOutput),
  pluginParams(ps),
  rate(rate),
  hwRate(hwRate),
  numChan(numChan),
  totalFrames(0),
  totalFeatures(0),
  plugin(0),
  plugbuf(0),
  winBuf(0),
  outputNo(-1),
  blockSize(0),
  stepSize(0),
  framesInPlugBuf(0),
  isOutputBinary(false),
  resampleDecim(hwRate / rate),
  resampleScale(1.0 / (32768.0 * resampleDecim)),
  resampleCountdown(resampleDecim),
  partialFrameSum(new int[numChan]),
  lastFrametimestamp(0),
  freqDomain(false)
{

  // try load the plugin and throw if we fail

  if (loadPlugin()) {
    delete_privates();
    throw std::runtime_error("Could not load plugin or plugin is not compatible");
  }
  for (int i=0; i < numChan; ++i)
    partialFrameSum[i] = 0;
};

PluginRunner::~PluginRunner() {
  delete_privates();
};

bool PluginRunner::addOutputListener(string label) {
  
  shared_ptr < Pollable > outl = boost::static_pointer_cast < Pollable > (lookupByNameShared(label));
  if (outl) {
    outputListeners[label] = outl;
    return true;
  } else {
    return false;
  }
};

void PluginRunner::removeOutputListener(string label) {
  outputListeners.erase(label);
};

void PluginRunner::removeAllOutputListeners() {
  outputListeners.clear();
};

bool
PluginRunner::queueOutput(const char *p, uint32_t len, double timestamp) {
  // alsaMinder has a block of data for us; it has already been
  // put into the plugin's buffers.

  RealTime rt = RealTime::fromSeconds( timestamp );
  outputFeatures(plugin->process(plugbuf, rt), label);
  return true;
};


// void PluginRunner::handleData(snd_pcm_sframes_t avail, int16_t *src0, int16_t *src1, int step, double frameTimestamp) {
//   // alsaHandler has some data for us.  If src1 is NULL, it's only one channel; otherwise it's two channels

//   int pfs0 = partialFrameSum[0];
//   int pfs1 = partialFrameSum[1];
//   int rc = resampleCountdown;
  
//   // get timestamp of first (hardware) frame in plugin's buffer
//   frameTimestamp -= (double) framesInPlugBuf / rate + (double) (resampleDecim - rc) / hwRate;

//   while (avail > 0) {
//     int hw_frames_to_copy = std::min((int) avail, (blockSize - framesInPlugBuf - 1) * resampleDecim + rc);
//     avail -= hw_frames_to_copy;
//     int decimated_frame_count = (hw_frames_to_copy + (resampleDecim - rc)) / resampleDecim;
//     float *pb0, *pb1;
//     pb0 = plugbuf[0] + framesInPlugBuf;

//     // choose an inner loop, depending on number of channels
//     if (src1) {
//       // two channels
//       pb1 = plugbuf[1] + framesInPlugBuf;
//       while (hw_frames_to_copy) {
//         pfs0 += *src0;
//         pfs1 += *src1;
//         src0 += step;
//         src1 += step;
//         --hw_frames_to_copy;
//         --rc;
//         if (rc == 0) {
//           *pb0++ = pfs0 * resampleScale;
//           *pb1++ = pfs1 * resampleScale;
//           pfs0 = pfs1 = 0;
//           rc = resampleDecim;
//         }
//       }
//     } else {
//       // one channel
//       while (hw_frames_to_copy) {
//         pfs0 += *src0;
//         src0 += step;
//         --hw_frames_to_copy;
//         --rc;
//         if (rc == 0) {
//           *pb0++ = pfs0 * resampleScale;
//           pfs0 = 0;
//           rc = resampleDecim;
//         }
//       }
//     }
//     framesInPlugBuf += decimated_frame_count;
//     totalFrames += decimated_frame_count;
//     if (framesInPlugBuf == blockSize) {
//       // time to call the plugin
          
//       RealTime rt = RealTime::fromSeconds( frameTimestamp );
//       outputFeatures(plugin->process(plugbuf, rt), label);

//       // shift samples if we're not advancing by a full
//       // block.
//       // Too bad the VAMP specs don't let the
//       // process() function deal with two segments for each
//       // buffer; then we wouldn't need these wastefull calls
//       // to memmove!  MAYBE FIXME: fake this by changing our own
//       // plugin to have blockSize = stepSize and deal
//       // internally with handling overlap!  Then fix this
//       // code so copying from alsa's mmap segment is done in
//       // one pass for all plugins waiting on a device, then
//       // the mmap segment is marked as available, then
//       // another pass calls process() on all plugins with
//       // full buffers.

//       if (stepSize < blockSize) {
//         memmove(&plugbuf[0][0], &plugbuf[0][stepSize], (blockSize - stepSize) * sizeof(float));
//         if (src1)
//           memmove(&plugbuf[1][0], &plugbuf[1][stepSize], (blockSize - stepSize) * sizeof(float));
//         framesInPlugBuf = blockSize - stepSize;
//         frameTimestamp += (double) stepSize / rate;
//       } else {
//         framesInPlugBuf = 0;
//         frameTimestamp += (double) blockSize / rate;
//       }
//     }
//   }
//   partialFrameSum[0] = pfs0;
//   partialFrameSum[1] = pfs1;
//   resampleCountdown = rc;
// };

void
PluginRunner::outputFeatures(Plugin::FeatureSet features, string prefix)
{  
  totalFeatures += features[outputNo].size();
  for (Plugin::FeatureList::iterator f = features[outputNo].begin(), g = features[outputNo].end(); f != g; ++f ) {
    if (isOutputBinary) {
      // NOTHING USES THIS CLAUSE RIGHT NOW (JMB 2014-06-03)
      // copy values as raw bytes to any outputListeners
      for (OutputListenerSet::iterator io = outputListeners.begin(); io != outputListeners.end(); /**/) {
        if (shared_ptr < Pollable > ptr = (io->second).lock()) {
          ptr->queueOutput((char *)& f->values[0], f->values.size() * sizeof(f->values[0]));
          ++io;
        } else {
          OutputListenerSet::iterator to_delete = io++;
          outputListeners.erase(to_delete);
        }
      }
    } else {
      ostringstream txt;
      txt.setf(ios::fixed,ios::floatfield);
      txt.precision(4); // 0.1 ms precision for timestamp

      RealTime rt;

      if (f->hasTimestamp) {
        rt = f->timestamp;
      }

      if (prefix.length())
        txt << prefix << ",";
      txt << (double) (rt.sec + rt.nsec / (double) 1.0e9);
      txt.unsetf(ios::floatfield); // now 4 digits total precision

      if (f->hasDuration) {
        rt = f->duration;
        txt << "," << rt.toString();
      }

      for (std::vector<float>::iterator v = f->values.begin(), w=f->values.end(); v != w; ++v) {
        txt << "," << *v;
      }

      txt << endl;

      // send output as text to any outputListeners
      for (OutputListenerSet::iterator io = outputListeners.begin(); io != outputListeners.end(); /**/) {
        if (shared_ptr < Pollable > ptr = (io->second).lock()) {
          string output = txt.str();
          ptr->queueOutput(output);
          ++io;
        } else {
          OutputListenerSet::iterator to_delete = io++;
          outputListeners.erase(to_delete);
        }
      }
    }
  }
};

string PluginRunner::toJSON() {
  ostringstream s;
  s << "{" 
    << "\"type\":\"PluginRunner\","
    << "\"devLabel\":\"" << devLabel << "\","
    << "\"libraryName\":\"" << pluginSOName << "\","
    << "\"pluginID\":\"" << pluginID << "\","
    << "\"pluginOutput\":\"" << pluginOutput << "\","
    << "\"totalFrames\":" << totalFrames << ","
    << "\"totalFeatures\":" << totalFeatures
    << "}";
  return s.str();
}

PluginLoader *PluginRunner::pluginLoader = 0;

/*
  Trivially implementing the following methods allow us to put
  PluginRunners in the same host container as TCPListeners,
  TCPConnections, and AlsaMinders.  It's an ugly design, but I
  couldn't think of a better one, and it makes for simpler code, as
  far as I can tell.  */

void PluginRunner::stop(double timeNow) {
  /* do nothing */
};

int
PluginRunner::start(double timeNow) {
  /* do nothing */
  return 0;
};

void
PluginRunner::setParameters(ParamSet &ps) {
  if (plugin)
    for (ParamSetIter it = ps.begin(); it != ps.end(); ++it)
      plugin->setParameter(it->first, it->second);
};

int
PluginRunner::getNumPollFDs() {
  /* do nothing */
  return 0;
};

int 
PluginRunner::getPollFDs (struct pollfd * pollfds) {
  /* do nothing */
  return 0;
};

void
PluginRunner::handleEvents (struct pollfd *pollfds, bool timedOut, double timeNow)
{
  /* do nothing */
};

float *
PluginRunner::hammingWindow(int N)
{
    // allocate and generate a float array of size N of
    // Hamming window coefficients

    float * window = fftwf_alloc_real(N);
    for (int i = 0; i < N; ++i) {
        window[i] = 0.54 - 0.46 * cosf(2 * M_PI * i / (N - 1));
    }
    return window;
}
