#include "AlsaMinder.hpp"

void AlsaMinder::delete_privates() {
  if (pcm) {
    snd_pcm_drop(pcm);
    snd_pcm_close(pcm);
    pcm = 0;
  }
  for (ListenerSet::iterator il = listeners.begin(); il != listeners.end(); /**/) {
    delete il->second;
    ListenerSet::iterator del = il++;
    listeners.erase(del);
  }
};
    
int AlsaMinder::open() {
  // open the audio device and set our default audio parameters
  // return 0 on success, non-zero on error;

  snd_pcm_hw_params_t *params;
  snd_pcm_sw_params_t *swparams;
  snd_pcm_access_mask_t *mask;
  snd_pcm_uframes_t boundary;

  snd_pcm_hw_params_alloca( & params);
  snd_pcm_sw_params_alloca( & swparams);
  snd_pcm_access_mask_alloca( & mask );
        
  snd_pcm_access_mask_none( mask);
  snd_pcm_access_mask_set( mask, SND_PCM_ACCESS_MMAP_INTERLEAVED);

  int rateDir = 1;

  if ((snd_pcm_open(& pcm, alsaDev.c_str(), SND_PCM_STREAM_CAPTURE, 0))
      || snd_pcm_hw_params_any(pcm, params)
      || snd_pcm_hw_params_set_access_mask(pcm, params, mask)
      || snd_pcm_hw_params_set_format(pcm, params, SND_PCM_FORMAT_S16_LE)
      || snd_pcm_hw_params_set_channels(pcm, params, numChan)
      || snd_pcm_hw_params_set_rate_resample(pcm, params, 0)
      || snd_pcm_hw_params_set_rate_last(pcm, params, & hwRate, & rateDir))
    return 1;

  // do our best with the supplied rate:
  // we use exact decimation to the closest rate, or the max if exceeded

  if (hwRate > rate && hwRate % rate != 0)
    rate = hwRate / (int) round((double)hwRate / rate); 
  else if (rate > hwRate)
    rate = hwRate;

  if(snd_pcm_hw_params_set_period_size_near(pcm, params, & period_frames, 0) < 0
      || snd_pcm_hw_params_set_buffer_size_near(pcm, params, & buffer_frames) < 0
      || snd_pcm_hw_params(pcm, params)
      || snd_pcm_sw_params_current(pcm, swparams)
      || snd_pcm_sw_params_set_tstamp_mode(pcm, swparams, SND_PCM_TSTAMP_ENABLE)
      || snd_pcm_sw_params_set_period_event(pcm, swparams, 1)
      // get the ring buffer boundary, and 
      || snd_pcm_sw_params_get_boundary	(swparams, &boundary)
      || snd_pcm_sw_params_set_stop_threshold (pcm, swparams, boundary)
      || snd_pcm_sw_params(pcm, swparams)
      || (numFD = snd_pcm_poll_descriptors_count (pcm)) < 0

      ) {
    return 1;
  } 
  return 0;
};

void AlsaMinder::do_stop(double timeNow) {
  Pollable::requestPollFDRegen();
  if (pcm) {
    snd_pcm_drop(pcm);
    snd_pcm_close(pcm);
    pcm = 0;
  }
  stopTimestamp = timeNow;
  stopped = true;
};

void AlsaMinder::stop(double timeNow) {
  shouldBeRunning = false;
  do_stop(timeNow);
};

int AlsaMinder::do_start(double timeNow) {
  if (!pcm && open())
    return 1;
  Pollable::requestPollFDRegen();
  snd_pcm_prepare(pcm);
  hasError = 0;
  snd_pcm_start(pcm);
  stopped = false;
  // set timestamps to:
  // - prevent warning about resuming after long pause
  // - allow us to notice no data has been received for too long after startup
  lastDataReceived = startTimestamp = timeNow; 
  return 0;
}

int AlsaMinder::start(double timeNow) {
  shouldBeRunning = true;
  return do_start(timeNow);
};

void AlsaMinder::removeListener(string & label) {
  listeners.erase(label);
};

AlsaMinder::AlsaMinder(const string &alsaDev, int rate, unsigned int numChan, const string &label, double now):
  Pollable(label),
  alsaDev(alsaDev),
  rate(rate),
  numChan(numChan),
  pcm(0),
  buffer_frames(BUFFER_FRAMES),
  period_frames(PERIOD_FRAMES),
  revents(0),
  totalFrames(0),
  startTimestamp(-1.0),
  stopTimestamp(now),
  lastDataReceived(-1.0),
  shouldBeRunning(false),
  stopped(true),
  hasError(0),
  numFD(0)
{
  if (open()) {
    // there was an error, so throw an exception
    delete_privates();
    throw std::runtime_error("Could not open audio device or could not set required parameters");
  }

};

AlsaMinder::~AlsaMinder() {
  delete_privates();
};

string AlsaMinder::about() {
  return "Device '" + label + "' = " + alsaDev;
};

string AlsaMinder::toJSON() {
  ostringstream s;
  s << "{" 
    << "\"type\":\"AlsaMinder\","
    << "\"device\":\"" << alsaDev << "\","
    << "\"rate\":" << rate << ","
    << "\"hwRate\":" << hwRate << ","
    << "\"numChan\":" << numChan << ","
    << setprecision(14)
    << "\"startTimestamp\":" << startTimestamp << ","
    << "\"stopTimestamp\":" << stopTimestamp << ","
    << "\"running\":" << (stopped ? "false" : "true") << ","
    << "\"hasError\":" << hasError << ","
    << "\"totalFrames\":" << totalFrames
    << "}";
  return s.str();
}

int AlsaMinder::getNumPollFDs () {
  return (pcm && shouldBeRunning) ? numFD : 0;
};

int AlsaMinder::getPollFDs (struct pollfd *pollfds) {
  // append pollfd(s) for this object to the specified vector
  // ALSA weirdness means there may be more than one fd per audio device
  if (pcm && shouldBeRunning) {
    if (numFD != snd_pcm_poll_descriptors(pcm, pollfds, numFD)) {
      std::ostringstream msg;
      msg << "{\"event\":\"devProblem\",\"error\":\"snd_pcm_poll_descriptors returned error.\",\"devLabel\":\"" << label << "\"}\n";
      Pollable::asyncMsg(msg.str());
      return 1;
    }
  }
  return 0;
}

void AlsaMinder::handleEvents ( struct pollfd *pollfds, bool timedOut, double timeNow) {
  if (!pcm)
    return;
  short unsigned revents;
  if (!timedOut) {
    int rv = snd_pcm_poll_descriptors_revents( pcm, pollfds, numFD, & revents);
    if (rv != 0) {
      throw std::runtime_error(about() + ": snd_pcm_poll_descriptors_revents returned error.\n");
    }
  } else {            
    revents = 0;
  }
  if (revents & (POLLIN | POLLPRI)) {
    // copy as much data as possible from mmap ring buffer
    // and inform any pluginRunners that we have data

    snd_pcm_sframes_t avail = snd_pcm_avail_update (pcm);
    if (avail < 0) {
      snd_pcm_recover(pcm, avail, 1);
      snd_pcm_prepare(pcm);
      hasError = 0;
      snd_pcm_start(pcm);
      startTimestamp = timeNow;

    } else if (avail > 0) {
      lastDataReceived = timeNow;

      double frameTimestamp;

      // get most recent period timestamp from ALSA
      snd_htimestamp_t ts;
      snd_pcm_uframes_t av;
      snd_pcm_htimestamp(pcm, &av, &ts);
      // ts is the time at which there were av frames available.
      // The timestamp for the last frame available is thus
      // later by (avail - av) / hwRate seconds.
      // We maintain the timestamp of this newest frame
      frameTimestamp = ts.tv_sec + (double) ts.tv_nsec / 1.0e9 + (double) (avail - av) / hwRate;

      // begin direct access to ALSA mmap buffers for the device
      const snd_pcm_channel_area_t *areas;
      snd_pcm_uframes_t offset;
      snd_pcm_uframes_t have = (snd_pcm_sframes_t) avail;

      int errcode;
      if ((errcode = snd_pcm_mmap_begin (pcm, & areas, & offset, & have))) {
        std::ostringstream msg;
        msg << "{\"event\":\"devProblem\",\"error\":\" snd_pcm_mmap_begin returned with error " << (-errcode) << "\",\"devLabel\":\"" << label << "\"}\n";
        Pollable::asyncMsg(msg.str());
        return;
      }
      avail = have;
   
      totalFrames += avail;
      sample_t * src0 = (sample_t *) (((unsigned char *) areas[0].addr) + areas[0].first / 8);
      int step = areas[0].step / (8 * sizeof(sample_t)); // FIXME:  hardcoding S16_LE assumption
      src0 += step * offset;

      // for each listener, push the data into its ring buffer
      // FIXME: we're assuming the samples coming from ALSA are interleaved.
      // This is true for all devices we've used so far (funcubedongles and
      // various USB microphones).

      for (ListenerSet::iterator il = listeners.begin(); il != listeners.end(); ++il) {
        circBuf * cb = il->second->getCircularBuffer();
        int take = std::min((int) cb->capacity(), (int) (avail * numChan));
#ifdef DEBUG
        std::cerr << "Size: " << cb->size() << " adding " << take << std::endl;
#endif
        cb->insert(cb->end(), src0, src0 + take);
      };

      /*
        Tell ALSA we're finished using its internal mmap buffer.
      */

      if (0 > snd_pcm_mmap_commit (pcm, offset, avail)) {
        std::ostringstream msg;
        msg << "{\"event\":\"devProblem\",\"error\":\" snd_pcm_mmap_commit returned with error " << (-errcode) << "\",\"devLabel\":\"" << label << "\"}\n";
        Pollable::asyncMsg(msg.str());
      }
  
      // now for each listener, send as many blocks as are available
      for (ListenerSet::iterator il = listeners.begin(); il != listeners.end(); /**/) {
        AudioAdapter * ptr = il->second;
        circBuf *cb = ptr->getCircularBuffer();
        int bs = ptr->getBlockSize();
        do {
          if (bs != 0 && cb->size() < bs * numChan)
            break;
          int discard = ptr->handleData (cb->array_one(), cb->array_two(), frameTimestamp - (cb->size() - 1.0) / hwRate);
          // discard is how many samples we can discard from the front of the ring buffer
          // if negative, the ultimate listener no longer exists, so we should remove it from here.
          if (discard < 0) {
            ListenerSet::iterator to_delete = il++;
            listeners.erase(to_delete);
            break;
            // FIXME: delete this listener from the set
          } else {
#ifdef DEBUG
            std::cerr << "Size: " << cb->size() << " delete " << discard << std::endl;
#endif
            cb->erase_begin(discard);
          }
        } while (cb->size()); // loop around again, in case we've received more than one block's worth of data.
        ++il;
      }
    } else if (shouldBeRunning && lastDataReceived >= 0 && timeNow - lastDataReceived > MAX_AUDIO_QUIET_TIME) {
      // this device appears to have stopped delivering audio; try restart it
      std::ostringstream msg;
      // generate a JSON fragment, and send as async message to control socket
      msg << "\"event\":\"devStalled\",\"error\":\"no data received for " << (timeNow - lastDataReceived) << " secs;\",\"devLabel\":\"" << label << "\"";
      Pollable::asyncMsg(msg.str());
      lastDataReceived = timeNow; // wait before next restart
      stop(timeNow);
      Pollable::requestPollFDRegen();
    }
  }
};

void
AlsaMinder::addListener (string & label, AudioAdapter *ad) {
  listeners.insert(std::pair < string, AudioAdapter *> (label, ad));
};
        
