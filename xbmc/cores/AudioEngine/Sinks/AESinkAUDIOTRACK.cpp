 /*
 *      Copyright (C) 2010-2013 Team XBMC
 *      http://xbmc.org
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with XBMC; see the file COPYING.  If not, see
 *  <http://www.gnu.org/licenses/>.
 *
 */

#include "AESinkAUDIOTRACK.h"
#include "cores/AudioEngine/Utils/AEUtil.h"
#include "cores/AudioEngine/Utils/AERingBuffer.h"
#include "android/activity/XBMCApp.h"
#include "settings/Settings.h"
#if defined(HAS_LIBAMCODEC)
#include "utils/AMLUtils.h"
#endif
#include "utils/log.h"
#include "utils/StringUtils.h"
#include "settings/AdvancedSettings.h"

#include "android/jni/AudioFormat.h"
#include "android/jni/AudioManager.h"
#include "android/jni/AudioTrack.h"
#include "android/jni/Build.h"
#define ANDROID_MAX_CHANNELS 8
static enum AEChannel AndroidChannelMap[ANDROID_MAX_CHANNELS + 1] = {
  AE_CH_FL      , AE_CH_FR      , AE_CH_FC      , AE_CH_LFE     , AE_CH_BL      , AE_CH_BR      , AE_CH_SL      , AE_CH_SR      ,
  AE_CH_NULL
};
static const unsigned int PassthroughSampleRates[] = { 8000, 11025, 16000, 22050, 24000, 32000, 41400, 48000, 88200, 96000, 176400, 192000 };

using namespace jni;

#if 0 //defined(__ARM_NEON__)
#include <arm_neon.h>
#include "utils/CPUInfo.h"

// LGPLv2 from PulseAudio
// float values from AE are pre-clamped so we do not need to clamp again here
static void pa_sconv_s16le_from_f32ne_neon(unsigned n, const float32_t *a, int16_t *b)
{
  unsigned int i;

  const float32x4_t half4     = vdupq_n_f32(0.5f);
  const float32x4_t scale4    = vdupq_n_f32(32767.0f);
  const uint32x4_t  mask4     = vdupq_n_u32(0x80000000);

  for (i = 0; i < (n & ~3); i += 4)
  {
    const float32x4_t v4 = vmulq_f32(vld1q_f32(&a[i]), scale4);
    const float32x4_t w4 = vreinterpretq_f32_u32(
      vorrq_u32(vandq_u32(vreinterpretq_u32_f32(v4), mask4), vreinterpretq_u32_f32(half4)));
    vst1_s16(&b[i], vmovn_s32(vcvtq_s32_f32(vaddq_f32(v4, w4))));
  }
  // leftovers
  for ( ; i < n; i++)
    b[i] = (int16_t) lrintf(a[i] * 0x7FFF);
}
#endif

CAEDeviceInfo CAESinkAUDIOTRACK::m_info;
CAEDeviceInfo CAESinkAUDIOTRACK::m_infoMC;
////////////////////////////////////////////////////////////////////////////////////////////
CAESinkAUDIOTRACK::CAESinkAUDIOTRACK()
{
  m_alignedS16 = NULL;
  m_min_frames = 0;
  m_sink_frameSize = 0;
  m_audiotrackbuffer_sec = 0.0;
  m_volume = 1.0;
  m_at_jni = NULL;
  m_frames_written = 0;
}

CAESinkAUDIOTRACK::~CAESinkAUDIOTRACK()
{
  Deinitialize();
}

bool CAESinkAUDIOTRACK::Initialize(AEAudioFormat &format, std::string &device)
{
  m_lastFormat  = format;
  m_format      = format;

  CAEDeviceInfo info;
  if (StringUtils::EqualsNoCase(device, "AudioTrackMC"))
  {
    info = m_infoMC;
    m_passthrough = false;
  }
  else
  {
    info = m_info;
    m_passthrough = AE_IS_RAW(m_format.m_dataFormat);
  }

  int stream = CJNIAudioManager::STREAM_MUSIC;
  int encoding = CJNIAudioFormat::ENCODING_PCM_16BIT;
  int channelConfig = CJNIAudioFormat::CHANNEL_OUT_STEREO;

  if (m_passthrough)
  {
    if (CJNIAudioFormat::ENCODING_IEC61937_16BIT != -1)  // OUYA
      encoding = CJNIAudioFormat::ENCODING_IEC61937_16BIT;
    else if (StringUtils::StartsWithNoCase(CJNIBuild::HARDWARE, "rk3") && StringUtils::StartsWithNoCase(CJNIBuild::MODEL, "neo-x"))  // Minix
      stream = CJNIAudioManager::STREAM_VOICE_CALL;
  }

  /*
#if defined(HAS_LIBAMCODEC)
  if (CSettings::Get().GetBool("videoplayer.useamcodec"))
    aml_set_audio_passthrough(m_passthrough);
#endif
*/

  // default to 44100, all android devices support it.
  // then check if we can support the requested rate.
  unsigned int sampleRate = 44100;
  for (size_t i = 0; i < info.m_sampleRates.size(); i++)
  {
    if (m_format.m_sampleRate == info.m_sampleRates[i])
    {
      sampleRate = m_format.m_sampleRate;
      break;
    }
  }
  m_format.m_sampleRate = sampleRate;

  if (!m_passthrough)
  {
    switch  (m_format.m_channelLayout.Count())
    {
      case 8:
        channelConfig = CJNIAudioFormat::CHANNEL_OUT_7POINT1;
        break;
      case 6:
        channelConfig = CJNIAudioFormat::CHANNEL_OUT_5POINT1;
        break;
      default:
        break;
    }
  }

  m_format.m_dataFormat     = AE_FMT_S16LE;
  m_format.m_channelLayout  = info.m_channels;
  m_format.m_frameSize      = m_format.m_channelLayout.Count() *
                              (CAEUtil::DataFormatToBits(m_format.m_dataFormat) / 8);
  int min_buffer_size       = CJNIAudioTrack::getMinBufferSize( m_format.m_sampleRate,
                                                                channelConfig,
                                                                encoding);
  if (min_buffer_size < 0)  // Unsupported sample rate
    return false;

  m_sink_frameSize          = m_format.m_channelLayout.Count() *
                              (CAEUtil::DataFormatToBits(m_format.m_dataFormat) / 8);
  m_min_frames              = min_buffer_size / m_sink_frameSize;
  m_audiotrackbuffer_sec    = (double)m_min_frames / (double)m_format.m_sampleRate;
  m_at_jni                  = new CJNIAudioTrack( stream,
                                                  m_format.m_sampleRate,
                                                  channelConfig,
                                                  encoding,
                                                  min_buffer_size,
                                                  CJNIAudioTrack::MODE_STREAM);
  m_format.m_frames         = m_min_frames / 2;

  m_format.m_frameSamples   = m_format.m_frames * m_format.m_channelLayout.Count();
  format                    = m_format;

  JNIEnv* jenv = xbmc_jnienv();
  // Set the initial volume
  float volume = 1.0;
  if (!m_passthrough)
    volume = m_volume;
  CXBMCApp::SetSystemVolume(jenv, volume);

  return true;
}

void CAESinkAUDIOTRACK::Deinitialize()
{
  if (!m_at_jni)
    return;

  m_at_jni->stop();
  m_at_jni->flush();
  m_at_jni->release();

  m_frames_written = 0;

  delete m_at_jni;
  m_at_jni = NULL;
}

double CAESinkAUDIOTRACK::GetDelay()
{
  if (!m_at_jni)
    return 0.0;

  // In their infinite wisdom, Google decided to make getPlaybackHeadPosition
  // return a 32bit "int" that you should "interpret as unsigned."  As such,
  // for wrap saftey, we need to do all ops on it in 32bit integer math.
  uint32_t head_pos = (uint32_t)m_at_jni->getPlaybackHeadPosition();

  double delay = (double)(m_frames_written - head_pos) / m_format.m_sampleRate;

  return delay;
}

double CAESinkAUDIOTRACK::GetLatency()
{
#if defined(HAS_LIBAMCODEC)
  if (aml_present())
    return 0.250;
#endif
  return 0.0;
}

double CAESinkAUDIOTRACK::GetCacheTotal()
{
  // total amount that the audio sink can buffer in units of seconds
  return m_audiotrackbuffer_sec;
}

CAEChannelInfo CAESinkAUDIOTRACK::GetChannelLayout(AEAudioFormat format)
{
  unsigned int count = 0;

       if (format.m_dataFormat == AE_FMT_AC3 ||
           format.m_dataFormat == AE_FMT_DTS ||
           format.m_dataFormat == AE_FMT_EAC3)
           count = 2;
  else if (format.m_dataFormat == AE_FMT_TRUEHD ||
           format.m_dataFormat == AE_FMT_DTSHD)
           count = 8;
  else
  {
    for (unsigned int c = 0; c < 8; ++c)
      for (unsigned int i = 0; i < format.m_channelLayout.Count(); ++i)
        if (format.m_channelLayout[i] == AndroidChannelMap[c])
        {
          count = c + 1;
          break;
        }
  }

  CAEChannelInfo info;
  for (unsigned int i = 0; i < count; ++i)
    info += AndroidChannelMap[i];

  return info;
}

// this method is supposed to block until all frames are written to the device buffer
// when it returns ActiveAESink will take the next buffer out of a queue
unsigned int CAESinkAUDIOTRACK::AddPackets(uint8_t *data, unsigned int frames, bool hasAudio, bool blocking)
{
  if (!m_at_jni)
    return INT_MAX;

  // write as many frames of audio as we can fit into our internal buffer.
  int written = 0;
  if (frames)
  {
    // android will auto pause the playstate when it senses idle,
    // check it and set playing if it does this. Do this before
    // writing into its buffer.
    if (m_at_jni->getPlayState() != CJNIAudioTrack::PLAYSTATE_PLAYING)
      m_at_jni->play();

    written = m_at_jni->write((char*)data, 0, frames * m_sink_frameSize);
    m_frames_written += written / m_sink_frameSize;
  }

  return (unsigned int)(written/m_sink_frameSize);
}

void CAESinkAUDIOTRACK::Drain()
{
  if (!m_at_jni)
    return;

  // TODO: does this block until last samples played out?
  // we should not return from drain as long the device is in playing state
  m_at_jni->stop();
  m_frames_written = 0;
}

bool CAESinkAUDIOTRACK::HasVolume()
{
  return true;
}

void  CAESinkAUDIOTRACK::SetVolume(float scale)
{
  if (!m_at_jni)
    return;

  m_volume = scale;
  if (!m_passthrough)
  {
    CXBMCApp::SetSystemVolume(xbmc_jnienv(), m_volume);
  }
}

void CAESinkAUDIOTRACK::EnumerateDevicesEx(AEDeviceInfoList &list, bool force)
{
  m_info.m_channels.Reset();
  m_info.m_dataFormats.clear();
  m_info.m_sampleRates.clear();

  m_info.m_deviceType = AE_DEVTYPE_HDMI;
  m_info.m_deviceName = "AudioTrack";
  m_info.m_displayName = "android";
  m_info.m_displayNameExtra = "audiotrack";
  for (int j = 0; j < 2; ++j)
      m_info.m_channels += AndroidChannelMap[j];
  for (unsigned int i=0; i<sizeof PassthroughSampleRates/sizeof *PassthroughSampleRates; i++)
    m_info.m_sampleRates.push_back(PassthroughSampleRates[i]);
  //m_info.m_sampleRates.push_back(CJNIAudioTrack::getNativeOutputSampleRate(CJNIAudioManager::STREAM_MUSIC));
  //m_info.m_sampleRates.push_back(48000);  // for passthrough
  m_info.m_dataFormats.push_back(AE_FMT_S16LE);
  m_info.m_dataFormats.push_back(AE_FMT_AC3);
  m_info.m_dataFormats.push_back(AE_FMT_DTS);
  m_info.m_dataFormats.push_back(AE_FMT_EAC3);
#if 0 //defined(__ARM_NEON__)
  if (g_cpuInfo.GetCPUFeatures() & CPU_FEATURE_NEON)
    m_info.m_dataFormats.push_back(AE_FMT_FLOAT);
#endif

  list.push_back(m_info);

  if (g_advancedSettings.m_androidfakeaudiodevices)
  {
    m_infoMC.m_channels.Reset();
    m_infoMC.m_dataFormats.clear();
    m_infoMC.m_sampleRates.clear();

    m_infoMC.m_deviceType = AE_DEVTYPE_PCM;
    m_infoMC.m_deviceName = "AudioTrackMC";
    m_infoMC.m_displayName = "android(Multi-channel)";
    m_infoMC.m_displayNameExtra = "audiotrack";
    for (int j = 0; j < ANDROID_MAX_CHANNELS; ++j)
      m_infoMC.m_channels += AndroidChannelMap[j];
    m_infoMC.m_sampleRates.push_back(44100);
    m_infoMC.m_sampleRates.push_back(48000);
    m_infoMC.m_dataFormats.push_back(AE_FMT_S16LE);

    list.push_back(m_infoMC);
  }
}

