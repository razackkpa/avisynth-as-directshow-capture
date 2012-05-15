#include <streams.h>

#include "PushSource.h"
#include "PushGuids.h"
#include "DibHelper.h"
#include <wmsdkidl.h>

/**********************************************
 *
 *  CPushPinDesktop Class
 *  
 *
 **********************************************/
#define MIN(a,b)  ((a) < (b) ? (a) : (b))  // danger! can evaluate "a" twice.

DWORD globalStart; // for some debug performance benchmarking
int countMissed = 0;
long fastestRoundMillis = 1000000;
long sumMillisTook = 0;

#ifdef _DEBUG 
  int show_performance = 1;
#else
  int show_performance = 0;
#endif

wchar_t out[1000];

// the default child constructor...
CPushPinDesktop::CPushPinDesktop(HRESULT *phr, CPushSourceDesktop *pFilter)
        : CSourceStream(NAME("What is this string for?"), phr, pFilter, L"Capture"),
        m_FramesWritten(0),
        m_iFrameNumber(0),
		m_pParent(pFilter),
		m_bFormatAlreadySet(false)
{

    // Get the device context of the main display, just to get some metrics for it...
	globalStart = GetTickCount();

    // Get the dimensions of the main desktop window as the default
    m_rScreen.left   = m_rScreen.top = 0;
	// TODO rdp

	WarmupCounter();
	LocalOutput(L"warmup the debugging message system");
	__int64 measureDebugOutputSpeed = StartCounter();
	LocalOutput(out);
	LocalOutput("writing a large-ish debug itself took: %.02Lf ms", GetCounterSinceStartMillis(measureDebugOutputSpeed));
	set_config_string_setting(L"last_init_config_was", out);
}


HRESULT CPushPinDesktop::FillBuffer(IMediaSample *pSample)
{
	//LocalOutput("called fillbuffer"); Encoder 4 does *not* get here.

	__int64 startThisRound = StartCounter();
	BYTE *pData;

    CheckPointer(pSample, E_POINTER);

    // Access the sample's data buffer
    pSample->GetPointer(&pData);

    // Make sure that we're still using video format
    ASSERT(m_mt.formattype == FORMAT_VideoInfo);

    VIDEOINFOHEADER *pVih = (VIDEOINFOHEADER*) m_mt.pbFormat;

	// for some reason the timings are messed up initially, as there's no start time at all for the first frame (?) we don't start in State_Running ?
	// race condition?
	// so don't do some calculations unless we're in State_Running
	FILTER_STATE myState;
	CSourceStream::m_pFilter->GetState(INFINITE, &myState);
	bool fullyStarted = myState == State_Running;
	
	boolean gotNew = false;
	
	// TODO rdp

	// capture how long it took before we add in our own arbitrary delay to enforce fps...
	long double millisThisRoundTook = GetCounterSinceStartMillis(startThisRound);
	fastestRoundMillis = min(millisThisRoundTook, fastestRoundMillis); // keep stats :)
	sumMillisTook += millisThisRoundTook;

	CRefTime now;
	CRefTime endFrame;
    CSourceStream::m_pFilter->StreamTime(now);

    // wait until we "should" send this frame out...
	if((now > 0) && (now < previousFrameEndTime)) { // now > 0 to accomodate for if there is no reference graph clock at all...also boot strap time ignore it :P
		while(now < previousFrameEndTime) { // guarantees monotonicity too :P
		  Sleep(1);
          CSourceStream::m_pFilter->StreamTime(now);
		}
		// avoid a tidge of creep since we sleep until [typically] just past the previous end.
		endFrame = previousFrameEndTime + m_rtFrameLength;
	    previousFrameEndTime = endFrame;
	    
	} else {
	  if(show_performance)
	    LocalOutput("it missed a frame--can't keep up %d", countMissed++); // we don't miss time typically I don't think, unless de-dupe is turned on, or aero, or slow computer, buffering problems downstream, etc.
	  // have to add a bit here, or it will always be "it missed some time" for the next round...forever!
	  endFrame = now + m_rtFrameLength;
	  // most of this stuff I just made up because it "sounded right"
	  //LocalOutput("checking to see if I can catch up again now: %llu previous end: %llu subtr: %llu %i", now, previousFrameEndTime, previousFrameEndTime - m_rtFrameLength, previousFrameEndTime - m_rtFrameLength);
	  if(now > (previousFrameEndTime - (long long) m_rtFrameLength)) { // do I need a long long cast?
		// let it pretend and try to catch up, it's not quite a frame behind
        previousFrameEndTime = previousFrameEndTime + m_rtFrameLength;
	  } else {
		endFrame = now + m_rtFrameLength/2; // ?? seems to work...I guess...
		previousFrameEndTime = endFrame;
	  }
	    
	}
	previousFrameEndTime = max(0, previousFrameEndTime);// avoid startup negatives, which would kill our math on the next loop...
    
	// LocalOutput("marking frame with timestamps: %llu %llu", now, endFrame);
    //pSample->SetTime((REFERENCE_TIME *) &now, (REFERENCE_TIME *) &endFrame);
	//pSample->SetMediaTime((REFERENCE_TIME *)&now, (REFERENCE_TIME *) &endFrame); //useless seemingly

	if(fullyStarted) {
      m_iFrameNumber++;
	}

	// Set TRUE on every sample for uncompressed frames http://msdn.microsoft.com/en-us/library/windows/desktop/dd407021%28v=vs.85%29.aspx
    pSample->SetSyncPoint(TRUE);

	// only set discontinuous for the first...I think...
	pSample->SetDiscontinuity(m_iFrameNumber <= 1);

    // the swprintf costs like 0.04ms (25000 fps LOL)
	m_fFpsSinceBeginningOfTime = ((double) m_iFrameNumber)/(GetTickCount() - globalStart)*1000;
	swprintf(out, L"done frame! total frames: %d this one %dx%d took: %.02Lfms, %.02f ave fps (%.02f is the theoretical max fps based on this round, ave. possible fps %.02f, fastest round fps %.02f, negotiated fps %.06f), frame missed %d", 
		m_iFrameNumber, m_iCaptureHeight, m_iCaptureWidth, millisThisRoundTook, m_fFpsSinceBeginningOfTime, 1.0*1000/millisThisRoundTook,   
		/* average */ 1.0*1000*m_iFrameNumber/sumMillisTook, 1.0*1000/fastestRoundMillis, GetFps(), countMissed);
#ifdef _DEBUG // probably not worth it but we do hit this a lot...hmm...
	LocalOutput(out);
	set_config_string_setting(L"frame_stats", out);
#endif
    return S_OK;
}

float CPushPinDesktop::GetFps() {
	return (float) (UNITS / m_rtFrameLength);
}

CPushPinDesktop::~CPushPinDesktop()
{   
	// They *should* call this...VLC does at least, correctly.

    // Release the device context stuff
    DbgLog((LOG_TRACE, 3, TEXT("Total no. Frames written %d"), m_iFrameNumber));
	set_config_string_setting(L"last_run_performance", out);

}


//
// DecideBufferSize
//
// This will always be called after the format has been sucessfully
// negotiated (this is negotiatebuffersize). So we have a look at m_mt to see what size image we agreed.
// Then we can ask for buffers of the correct size to contain them.
//
HRESULT CPushPinDesktop::DecideBufferSize(IMemAllocator *pAlloc,
                                      ALLOCATOR_PROPERTIES *pProperties)
{
    CheckPointer(pAlloc,E_POINTER);
    CheckPointer(pProperties,E_POINTER);

    CAutoLock cAutoLock(m_pFilter->pStateLock());
    HRESULT hr = NOERROR;

    VIDEOINFO *pvi = (VIDEOINFO *) m_mt.Format();
	BITMAPINFOHEADER header = pvi->bmiHeader;
	ASSERT(header.biPlanes == 1); // sanity check
	
	ASSERT(header.biHeight > 0); // sanity check
	ASSERT(header.biWidth > 0); // sanity check
	pProperties->cbBuffer = header.biSizeImage; // TODO check this, with RGB: vlc -vvv dshow:// :dshow-vdev="screen-capture-recorder" :dshow-adev --sout  "#transcode{venc=theora,vcodec=theo,vb=512,scale=0.7,acodec=vorb,ab=128,channels=2,samplerate=44100,audio-sync}:standard{access=file,mux=ogg,dst=test.ogv}" with 10x10 or 1000x1000

    pProperties->cBuffers = 1; // 2 here doesn't seem to help the crashes...

    // Ask the allocator to reserve us some sample memory. NOTE: the function
    // can succeed (return NOERROR) but still not have allocated the
    // memory that we requested, so we must check we got whatever we wanted.
    ALLOCATOR_PROPERTIES Actual;
    hr = pAlloc->SetProperties(pProperties,&Actual);
    if(FAILED(hr))
    {
        return hr;
    }

    // Is this allocator unsuitable?
    if(Actual.cbBuffer < pProperties->cbBuffer)
    {
        return E_FAIL;
    }

	// now some "once per run" setups
	// ...
	
    return NOERROR;

} // DecideBufferSize