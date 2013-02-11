/* -LICENSE-START-
** Copyright (c) 2009 Blackmagic Design
**
** Permission is hereby granted, free of charge, to any person or organization
** obtaining a copy of the software and accompanying documentation covered by
** this license (the "Software") to use, reproduce, display, distribute,
** execute, and transmit the Software, and to prepare derivative works of the
** Software, and to permit third-parties to whom the Software is furnished to
** do so, all subject to the following:
** 
** The copyright notices in the Software and this entire statement, including
** the above license grant, this restriction and the following disclaimer,
** must be included in all copies of the Software, in whole or in part, and
** all derivative works of the Software, unless such copies or derivative
** works are solely in the form of machine-executable object code generated by
** a source language processor.
** 
** THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
** IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
** FITNESS FOR A PARTICULAR PURPOSE, TITLE AND NON-INFRINGEMENT. IN NO EVENT
** SHALL THE COPYRIGHT HOLDERS OR ANYONE DISTRIBUTING THE SOFTWARE BE LIABLE
** FOR ANY DAMAGES OR OTHER LIABILITY, WHETHER IN CONTRACT, TORT OR OTHERWISE,
** ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
** DEALINGS IN THE SOFTWARE.
** -LICENSE-END-
*/

#include "../include/DecklinkInput.h"

using namespace std;

DeckLinkInputDelegate::DeckLinkInputDelegate(pthread_cond_t* m_sleepCond, IDeckLinkOutput* m_deckLinkOutput, IDeckLinkVideoConversion* m_deckLinkConverter)
 : m_refCount(0), g_timecodeFormat(0), frameCount(0), final_frameCount(0)
{
	sleepCond = m_sleepCond;
	deckLinkOutput = m_deckLinkOutput;
	deckLinkConverter = m_deckLinkConverter;

	pthread_mutex_init(&m_mutex, NULL);
}

DeckLinkInputDelegate::~DeckLinkInputDelegate()
{
	pthread_mutex_destroy(&m_mutex);
}

ULONG DeckLinkInputDelegate::AddRef(void)
{
	pthread_mutex_lock(&m_mutex);
		m_refCount++;
	pthread_mutex_unlock(&m_mutex);

	return (ULONG)m_refCount;
}

ULONG DeckLinkInputDelegate::Release(void)
{
	pthread_mutex_lock(&m_mutex);
		m_refCount--;
	pthread_mutex_unlock(&m_mutex);

	if (m_refCount == 0)
	{
		delete this;
		return 0;
	}

	return (ULONG)m_refCount;
}

tr1::shared_ptr<openshot::Frame> DeckLinkInputDelegate::GetFrame(int requested_frame)
{
	tr1::shared_ptr<openshot::Frame> f;

	#pragma omp critical (blackmagic_input_queue)
	{
		if (final_frames.size() > 0)
		{
			//cout << "remaining: " << final_frames.size() << endl;
			f = final_frames.front();
			final_frames.pop_front();
		}
	}


	return f;
}

HRESULT DeckLinkInputDelegate::VideoInputFrameArrived(IDeckLinkVideoInputFrame* videoFrame, IDeckLinkAudioInputPacket* audioFrame)
{
	// Handle Video Frame
	if(videoFrame)
	{	

		if (videoFrame->GetFlags() & bmdFrameHasNoInputSource)
		{
			//fprintf(stderr, "Frame received (#%lu) - No input signal detected\n", frameCount);
		}
		else
		{
			const char *timecodeString = NULL;
			if (g_timecodeFormat != 0)
			{
				IDeckLinkTimecode *timecode;
				if (videoFrame->GetTimecode(g_timecodeFormat, &timecode) == S_OK)
				{
					timecode->GetString(&timecodeString);
				}
			}

			//fprintf(stderr, "Frame received (#%lu) [%s] - Size: %li bytes\n",
			//	frameCount,
			//	timecodeString != NULL ? timecodeString : "No timecode",
			//	videoFrame->GetRowBytes() * videoFrame->GetHeight());

			if (timecodeString)
				free((void*)timecodeString);

			// Create a new copy of the YUV frame object
			IDeckLinkMutableVideoFrame *m_yuvFrame = NULL;

			int width = videoFrame->GetWidth();
			int height = videoFrame->GetHeight();

			HRESULT res = deckLinkOutput->CreateVideoFrame(
									width,
									height,
									videoFrame->GetRowBytes(),
									bmdFormat8BitYUV,
									bmdFrameFlagDefault,
									&m_yuvFrame);

			// Copy pixel and audio to copied frame
			void *frameBytesSource;
			void *frameBytesDest;
			videoFrame->GetBytes(&frameBytesSource);
			m_yuvFrame->GetBytes(&frameBytesDest);
			memcpy(frameBytesDest, frameBytesSource, videoFrame->GetRowBytes() * height);

			// Add raw YUV frame to queue
			raw_video_frames.push_back(m_yuvFrame);

			// Process frames once we have a few (to take advantage of multiple threads)
			if (raw_video_frames.size() >= omp_get_num_procs())
			{

//omp_set_num_threads(1);
omp_set_nested(true);
#pragma omp parallel
{
#pragma omp single
{
				// Temp frame counters (to keep the frames in order)
				frameCount = 0;

				// Loop through each queued image frame
				while (!raw_video_frames.empty())
				{
					// Get front frame (from the queue)
					IDeckLinkMutableVideoFrame* frame = raw_video_frames.front();
					raw_video_frames.pop_front();

					// declare local variables (for OpenMP)
					IDeckLinkOutput *copy_deckLinkOutput(deckLinkOutput);
					IDeckLinkVideoConversion *copy_deckLinkConverter(deckLinkConverter);
					unsigned long copy_frameCount(frameCount);

					#pragma omp task firstprivate(copy_deckLinkOutput, copy_deckLinkConverter, frame, copy_frameCount)
					{
						// *********** CONVERT YUV source frame to RGB ************
						void *frameBytes;
						void *audioFrameBytes;

						// Create a new RGB frame object
						IDeckLinkMutableVideoFrame *m_rgbFrame = NULL;

						int width = videoFrame->GetWidth();
						int height = videoFrame->GetHeight();

						HRESULT res = copy_deckLinkOutput->CreateVideoFrame(
												width,
												height,
												width * 4,
												bmdFormat8BitARGB,
												bmdFrameFlagDefault,
												&m_rgbFrame);

						if(res != S_OK)
							cout << "BMDOutputDelegate::StartRunning: Error creating RGB frame, res:" << res << endl;

						// Create a RGB version of this YUV video frame
						copy_deckLinkConverter->ConvertFrame(frame, m_rgbFrame);

						// Get RGB Byte array
						m_rgbFrame->GetBytes(&frameBytes);

						// *********** CREATE OPENSHOT FRAME **********
						tr1::shared_ptr<openshot::Frame> f(new openshot::Frame(frameCount, width, height, "#000000", 2048, 2));

						// Add Image data to openshot frame
						f->AddImage(width, height, "ARGB", Magick::CharPixel, (uint8_t*)frameBytes);

						// TEST EFFECTS
						f->TransparentColors("#8fa09a", 20.0);

						#pragma omp critical (blackmagic_input_queue)
						{
							// Add processed frame to cache (to be recalled in order after the thread pool is done)
							temp_cache.Add(copy_frameCount, f);
						}

						// Release RGB data
						if (m_rgbFrame)
							m_rgbFrame->Release();
						// Release RGB data
						if (frame)
							frame->Release();

					} // end task

					// Increment frame count
					frameCount++;

				} // end while

} // omp single
} // omp parallel

			// Add frames to final queue (in order)
			for (int z = 0; z < frameCount; z++)
			{
				if (temp_cache.Exists(z))
				{
					tr1::shared_ptr<openshot::Frame> f = temp_cache.GetFrame(z);

					// Add to final queue
					final_frames.push_back(f);
				}
			}

			// Clear temp cache
			temp_cache.Clear();

			// Don't keep too many frames (remove old frames)
			while (final_frames.size() > 20)
				// Remove oldest frame
				final_frames.pop_front();


			} // if size > num processors
		} // has video source
	} // if videoFrame

    return S_OK;
}

HRESULT DeckLinkInputDelegate::VideoInputFormatChanged(BMDVideoInputFormatChangedEvents events, IDeckLinkDisplayMode *mode, BMDDetectedVideoInputFormatFlags)
{
    return S_OK;
}



