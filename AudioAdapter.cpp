#include "AudioAdapter.hpp"

AudioAdapter::AudioAdapter (int rate, int hwRate, int numChan, int maxFrames, AudioAdapter::OutputType ot, int blockSize, int stepSize, string &listenerLabel, float ** buffs, bool writeWavFileHeader, int numFrames) :
  rate(rate),
  hwRate(hwRate),
  numChan(numChan),
  maxFrames (maxFrames),
  ot(ot),
  blockSize(blockSize),
  stepSize(stepSize),
  numFrames(numFrames),
  listenerLabel(listenerLabel),
  buffs(buffs),
  writeWavFileHeader(writeWavFileHeader),
  cb(PERIOD_FRAMES * 2 * numChan),
  downSampleBuf(0),
  winBuf(0),
  fftInput(0)
{
  downSampleFactor = hwRate / rate;
  if (ot == OT_INT && downSampleFactor > 1) {
    downSampleBuf = new int16_t [numChan * maxFrames];
  }
  downSampleCount = downSampleFactor;
  for (int j = 0; j < numChan; ++j)
    downSampleAccum[j] = 0;

  // maybe output .WAV header
  if (writeWavFileHeader) {
    Pollable *ptr = Pollable::lookupByNameShared(listenerLabel).get();
    if (ptr) {
      // default: if numFrames is zero, use  max possible frames in .WAV header
      // FIXME: hardcoded S16_LE format
      WavFileHeader hdr(rate, numChan, numFrames ? numFrames : 0x7ffffffe / 2);
      ptr->queueOutput(hdr.address(), hdr.size());
    }
  }

  numOutChan = (ot == OT_FM || ot == OT_SPECTRUM) ? 1 : numChan;

  // maybe allocate output buffers
  if (buffs) {
    weOwnBuffs = false;
  } else {
    weOwnBuffs = true;
    buffs = new float * [numOutChan];
    for (int i = 0; i < numOutChan; ++i) 
      // output buffer for FFT requires 2 extra slots
      buffs[i] = fftwf_alloc_real(blockSize + ot == OT_SPECTRUM ? 2 : 0);
  }

  // maybe allocate windowing function buffer and fftwf plan
  if (ot == OT_SPECTRUM) {
    winBuf = hammingWindow (blockSize);
    fftInput = (float *) fftwf_alloc_real (blockSize);
    // single plan will be used with different output arrays
    fftPlan = fftwf_plan_dft_r2c_1d( blockSize, fftInput, (fftwf_complex * ) buffs[0], FFTW_PATIENT); // '-1' for forward FFT plan
  }
}

AudioAdapter::~AudioAdapter() {
  if (weOwnBuffs && buffs) {
    for (int i = 0; i < numOutChan; ++i) 
      fftwf_free(buffs[i]);
    delete [] buffs;
    buffs = 0;
  }
  if (winBuf)
    fftwf_free(winBuf);

  if (downSampleBuf)
    delete [] downSampleBuf;

  if (fftInput)
    fftwf_free(fftInput);

  if (ot == OT_SPECTRUM)
    fftwf_destroy_plan(fftPlan);
};

int
AudioAdapter::handleData(circBuf::array_range a1, circBuf::array_range a2, double frameTimestamp)
{
  int avail = a1.second + a2.second;

  Pollable *ptr = Pollable::lookupByNameShared(listenerLabel).get();

  // if insufficient data or consumer no longer eixsts, consume no samples and do nothing
  if (avail < downSampleFactor || ! ptr)
    return 0;

  int useFrames = (avail / numChan / downSampleFactor) * downSampleFactor;
  if (blockSize > 0 && useFrames > blockSize)
    useFrames = blockSize;
    
  int useSeg[2];
  useSeg[0] = std::min(useFrames, (int)(a1.second / numChan));  // frames to use from first segment
  useSeg[1] = useFrames - useSeg[0]; // frames to use from second segment
  sample_t * src = a1.first;

  switch(ot) {
  case OT_INT:
    // downsample if necessary, then queue output
    {

      if (downSampleFactor > 1) {
        int k = 0;
            
        for(int s=0, use; s < 2 && (use=useSeg[s]); src=a2.first, ++s) {
          for (int i=0; i < use; ++i) {
            for (int j = 0; j < numChan; ++j)
              downSampleAccum[j] += *src++;
            if (0 == --downSampleCount) {
              downSampleCount = downSampleFactor;
              for (int j = 0; j < numChan; ++j) {
                downSampleBuf[k++] = (downSampleAccum[j] + downSampleFactor / 2) / downSampleFactor;
                downSampleAccum[j] = 0;
              }
            }
          }
        }
        ptr->queueOutput((const char *) downSampleBuf, k * sizeof(sample_t));
      } else {
        for(int s=0, use; s < 2 && (use=useSeg[s]); src=a2.first, ++s)
          ptr->queueOutput((const char *) src, use * numChan * sizeof(sample_t));
      }
      return useFrames * numChan;  // the number of samples which can now be discarded
    }
    break;

  case OT_FLOAT:
    // downsample, then queue a block of output
    {
      int k = 0;
      float convFactor = 1.0 / (32767 * downSampleFactor);

      if (downSampleFactor > 1) {
        for(int s=0, use; s < 2 && (use=useSeg[s]); src=a2.first, ++s) {
          for (int i=0; i < use; ++i) {
            for (int j = 0; j < numChan; ++j)
              downSampleAccum[j] += *src++;
            if (0 == --downSampleCount) {
              downSampleCount = downSampleFactor;
              for (int j = 0; j < numChan; ++j) {
                buffs[j][k] = downSampleAccum[j] * convFactor;
                downSampleAccum[j] = 0;
              }
              ++k;
            }
          }
        }
      } else {
        for(int s=0, use; s < 2 && (use=useSeg[s]); src=a2.first, ++s) {
          for (int i=0; i < use; ++i) {
            for (int j = 0; j < numChan; ++j)
              buffs[j][i] = *src++ * convFactor;
          }
        }
      }
      ptr->queueOutput((const char *)buffs[0], blockSize, frameTimestamp);
      return stepSize * numChan; // NB: stepSize, not block size, since we want caller to preserve the overlap samples
      // DONE
    }
    break;
    
  case OT_SPECTRUM:
  // WILL USE: void fftw_execute_dft_r2c(     const fftw_plan p,     double *in, fftw_complex *out);
    {
    }
    break;

  case OT_FM:
    {
// FM demod:
        // if (numChan == 2 && demodFMForRaw) {
        //   // do FM demodulation with simple but expensive arctan!
        //   float dthetaScale = hwRate / (2 * M_PI) / 75000.0 * 32767.0;
        //   int16_t * samps = (int16_t *) src0;
        //   for (int i=0; i < avail; ++i) {
        //     // get phase angle in -pi..pi
        //     float theta = atan2f(samps[2*i], samps[2*i+1]);
        //     float dtheta = theta - demodFMLastTheta;
        //     demodFMLastTheta = theta;
        //     if (dtheta > M_PI) {
        //       dtheta -= 2 * M_PI;
        //     } else if (dtheta < -M_PI) {
        //       dtheta += 2 * M_PI;
        //     }
        //     rawSamples[i] = roundf(dthetaScale * dtheta);
        //   }
    }
    break;

  }
  return avail; // pretend we've consumed all frames
};
  
bool
AudioAdapter::setOutputType(OutputType ot) {
  // all we allow is to toggle between INT and FM,
  // or to leave the output type unchanged
  if ( (ot == OT_INT && this->ot == OT_FM) ||
       (ot == OT_FM && this->ot == OT_INT)
       || ot == this->ot
       ) {
    this->ot = ot;
    return true;
  }
  return false;
};


float *
AudioAdapter::hammingWindow(int N)
{
    // allocate and generate a float array of size N of
    // Hamming window coefficients

    float * window = fftwf_alloc_real(N);
    for (int i = 0; i < N; ++i) {
        window[i] = 0.54 - 0.46 * cosf(2 * M_PI * i / (N - 1));
    }
    return window;
}
