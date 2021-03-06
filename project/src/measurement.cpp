#include <vector>
#include <iostream>
#include <cstring>
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include "math.h"

#include "linux_utils.h"
#include "picoscope.h"
#include "measurement.h"
#include "channel.h"
#include "timing.h"
#include "log.h"

#include "picoStatus.h"
#include "ps4000Api.h"
#include "ps6000Api.h"

Measurement::Measurement(Picoscope *p)
{
	FILE_LOG(logDEBUG3) << "Measurement::Measurement (Picoscope=" << p << ")";

	int i;

	picoscope = p;
	trigger = NULL;
	is_triggered = false;
	max_memory_consumption    = 0;
	max_trace_length_to_fetch = 0;
	ntraces = 1;
	max_traces_to_fetch = 1;
	timebase_reported_by_osciloscope = 0.0;
	use_signal_generator = false;
	rate_per_second = 0.0;

	for(i=0; i<GetNumberOfChannels(); i++) {
		// initialize the channels
		FILE_LOG(logDEBUG4) << "Measurement::Measurement - initialize channel nr. " << i;
		channels[i]=new Channel(i, this);
		data[i] = NULL;
		data_allocated[i] = false;
		data_length[i] = 0;
	}
	// only a temporary setting; this needs to be fixed if more than a single channel is enabled
	timebase = 0UL;
}

Measurement::~Measurement()
{
	FILE_LOG(logDEBUG3) << "Measurement::~Measurement";

	int i;
	for(i=0; i<GetNumberOfChannels(); i++) {
		// delete the channels
		delete channels[i];
		// delete data
		if(data_allocated[i]) {
			delete [] data[i];
			// not that it really matters now when the object is gone anyway
			data_allocated[i] = false;
			data_length[i] = 0;
		}
	}
	if(trigger != NULL) {
		delete trigger;
	}
}
// TODO: improve this; check for number of channels
void Measurement::EnableChannels(bool a, bool b, bool c, bool d)
{
	FILE_LOG(logDEBUG3) << "Measurement::EnableChannels (A=" << a << ",B=" << b << ",C=" << c << ",D=" << d << ")";

	/*if(GetSeries() == PICO_4000) {
		if(a) channels[PS4000_CHANNEL_A].Enable(); else channels[PS4000_CHANNEL_A].Disable();
		if(b) channels[PS4000_CHANNEL_B].Enable(); else channels[PS4000_CHANNEL_B].Disable();
		if(c) channels[PS4000_CHANNEL_C].Enable(); else channels[PS4000_CHANNEL_C].Disable();
		if(d) channels[PS4000_CHANNEL_D].Enable(); else channels[PS4000_CHANNEL_D].Disable();
	}*/
	if(a) GetChannel(0)->Enable(); else GetChannel(0)->Disable();
	if(b) GetChannel(1)->Enable(); else GetChannel(1)->Disable();
	if(c) GetChannel(2)->Enable(); else GetChannel(2)->Disable();
	if(d) GetChannel(3)->Enable(); else GetChannel(3)->Disable();

	FILE_LOG(logDEBUG4) << "Measurement::EnableChannels (A=" << GetChannel(0)->IsEnabled() << ",B=" << GetChannel(1)->IsEnabled() << ",C=" << GetChannel(2)->IsEnabled() << ",D=" << GetChannel(3)->IsEnabled() << ")";

	// if memory constraint has been set, make sure that
	if(GetMaxMemoryConsuption() != 0) {
		SetMaxMemoryConsumption(max_memory_consumption);
	}
}

// set the maximum possible timebase; TODO: this may depend on the number of channels,
// so this is not really safe
// void SetMaxTimebase()
// {
// 	timebase = 0UL;
// }

// TODO: the largest value for series 4000 is 54s which doesn't fit into unsigned long!!!
void Measurement::SetTimebaseInPs(unsigned long picoseconds)
{
	FILE_LOG(logDEBUG3) << "Measurement::SetTimebaseInPs (ps=" << picoseconds << ")";

	unsigned long i, result;

	if(GetSeries() == PICO_6000) {
		// it doesn't go beyond a certain minimum level
		if(picoseconds < 200) {
			picoseconds = 200;
		}
		if(picoseconds < 6400) {
			// it could be round() instead of floor()
			timebase = (unsigned long)floor(log2((double)(picoseconds/200)));
		} else {
			timebase = (unsigned long)(picoseconds/6400+4);
		}
	} else { /* 4223, 4224, 4423, 4424 only */
		if(picoseconds < 12500) {
			picoseconds = 12500;
		}
		if(picoseconds < 100000) {
			timebase = (unsigned long)floor(log2((double)(picoseconds/12500)));
		} else {
			timebase = (unsigned long)(picoseconds/50000+1);
		}
	}
}

void Measurement::SetTimebaseInNs(unsigned long nanoseconds)
{
	FILE_LOG(logDEBUG3) << "Measurement:SetTimebaseInNs (nanoseconds=" << nanoseconds << ")";

	// TODO: if(nanoseconds<...)
	SetTimebaseInPs(nanoseconds*1000UL);
	// else
}

double Measurement::GetTimebaseInNs()
{
	FILE_LOG(logDEBUG3) << "Measurement::GetTimebaseInNs";

	if(GetSeries() == PICO_6000) {
		if(timebase<6) {
			return 0.2*(1<<timebase);
		} else {
			return (timebase-4.0)*6.4;
		}
	} else {
		if(timebase<4) {
			return 12.5*(1<<timebase);
		} else {
			return (timebase-1.0)*50.0;
		}
	}
	return 0;
}

// when multiple channels are enabled we cannot have the highest sampling rate
void Measurement::FixTimebase()
{
	FILE_LOG(logDEBUG3) << "Measurement::FixTimebase";

	int n = GetNumberOfEnabledChannels();
	// when 3-4 channels are enabled, we need to set at least timebase=2 (4 times slower sampling)
	if(n >= 3) {
		if(timebase < 2UL) {
			timebase = 2UL;
		}
	} else if(n == 2) {
		if((GetChannel(0)->IsEnabled() && GetChannel(1)->IsEnabled()) ||
		   (GetChannel(2)->IsEnabled() && GetChannel(3)->IsEnabled())) {
			if(timebase < 2UL) {
				timebase = 2UL;
			}
		} else {
			if(timebase < 1UL) {
				timebase = 1UL;
			}
		}
	}
}

// this sets the timebase to picoscope
void Measurement::SetTimebaseInPicoscope()
{
	FILE_LOG(logDEBUG3) << "Measurement::SetTimebaseInPicoscope";

	float time_interval_ns;

	// 4000
	if(GetSeries() == PICO_4000) {
		FILE_LOG(logDEBUG2) << "ps4000GetTimebase2(handle=" << GetHandle() << ", timebase=" << GetTimebase() << ", length=" << GetLength() << ", &time_interval_ns, oversample=0, maxSamples=NULL, segmentIndex=0)";
		GetPicoscope()->SetStatus(ps4000GetTimebase2(
			GetHandle(),       // handle
			GetTimebase(),     // timebase
			GetLength(),       // noSamples
			&time_interval_ns, // timeIntervalNanoseconds
			0,                 // oversample (TODO if needed)
			NULL,              // maxSamples - the maximum number of samples available
			0));               // segmentIndex - the index of the memory segment to use
	// 6000
	} else {
		FILE_LOG(logDEBUG2) << "ps6000GetTimebase2(handle=" << GetHandle() << ", timebase=" << GetTimebase() << ", length=" << GetLength() << ", &time_interval_ns, oversample=0, maxSamples=NULL, segmentIndex=0)";
		GetPicoscope()->SetStatus(ps6000GetTimebase2(
			GetHandle(),       // handle
			GetTimebase(),     // timebase
			GetLength(),       // noSamples
			&time_interval_ns, // timeIntervalNanoseconds
			0,                 // oversample (TODO if needed)
			NULL,              // maxSamples - the maximum number of samples available
			0));               // segmentIndex - the index of the memory segment to use
		FILE_LOG(logDEBUG2) << "-> time_interval_ns=" << time_interval_ns << " ns";
	}
	if(GetPicoscope()->GetStatus() != PICO_OK) {
		std::cerr << "Unable to set the requested timebase (" << GetTimebase()
		          << ") and sample number (" << GetLength() << ")" << std::endl;
		throw Picoscope::PicoscopeException(GetPicoscope()->GetStatus());
	}

	std::cerr << "-- Setting timebase; the interval will be " << time_interval_ns << " ns\n";
	timebase_reported_by_osciloscope = (double)time_interval_ns;
}

void Measurement::SetLength(unsigned long l)
{
	FILE_LOG(logDEBUG3) << "Measurement::SetLength (length=" << l << ")";

	length = l;
}

void Measurement::SetNTraces(unsigned long n)
{
	FILE_LOG(logDEBUG3) << "Measurement::SetNTraces (n=" << n << ")";
	ntraces = n;
}


unsigned long Measurement::GetLengthBeforeTrigger()
{
	FILE_LOG(logDEBUG3) << "Measurement::GetLengthBeforeTrigger";

	double x_fraction;
	if(IsTriggered()) {
		x_fraction = GetTrigger()->GetXFraction();
		if(x_fraction>=0 && x_fraction<=1) {
			return (unsigned long)round((double)GetLength()*x_fraction);
		// we didn't allow that when creating new trigger, but we could
		// the idea is to use a number of samples instead of a fraction
		} else if(x_fraction>=0 && x_fraction<=GetLength()) {
			return (unsigned long)round(x_fraction);
		} else {
			throw("invalid value of x fraction of a simple trigger.\n");
		}
	} else {
		return 0UL;
	}
}
unsigned long Measurement::GetLengthAfterTrigger()
{
	FILE_LOG(logDEBUG3) << "Measurement::GetLengthAfterTrigger";

	if(IsTriggered()) {
		return GetLength()-GetLengthBeforeTrigger();
	} else {
		return GetLength();
	}
}

// sets the maximum allowed memory consumption
// and the maximum trace length to be read at once
void Measurement::SetMaxMemoryConsumption(unsigned long bytes)
{
	FILE_LOG(logDEBUG3) << "Measurement::SetMaxMemoryConsumption (bytes=" << bytes << ")";

	int enabled_channels = GetNumberOfEnabledChannels();
	FILE_LOG(logDEBUG4) << "Measurement::SetMaxMemoryConsumption - enabled_channels=" << enabled_channels;
	unsigned long single_trace_bytes;

	max_memory_consumption    = bytes;
	if (enabled_channels > 0) {
		max_trace_length_to_fetch = bytes/(sizeof(short)*enabled_channels);
		FILE_LOG(logDEBUG4) << "Measurement::SetMaxMemoryConsumption - max_trace_length_to_fetch=" << max_trace_length_to_fetch;
	} else {
		max_trace_length_to_fetch = bytes/sizeof(short);
		FILE_LOG(logDEBUG4) << "Measurement::SetMaxMemoryConsumption - max_trace_length_to_fetch=" << max_trace_length_to_fetch << " (no enabled channels)";
	}
	if(GetNTraces() > 1) {
		single_trace_bytes = sizeof(short)*enabled_channels*GetLength();
		if(GetNTraces()*single_trace_bytes < max_memory_consumption) {
			max_traces_to_fetch = GetNTraces();
		} else {
			max_traces_to_fetch = (unsigned long)floor(max_memory_consumption/single_trace_bytes);
			FILE_LOG(logDEBUG4) << "Measurement::SetMaxMemoryConsumption - max_traces_to_fetch=" << max_traces_to_fetch << " (above limit for max_memory_consumption=" << max_memory_consumption << ")";
		}
	}
	assert(enabled_channels<=4);
}

void Measurement::AllocateMemoryBlock(unsigned long bytes)
{
	FILE_LOG(logDEBUG3) << "Measurement::AllocateMemoryBlock (bytes=" << bytes << ")";

	int i;
	unsigned long maxlen;

	SetMaxMemoryConsumption(bytes);
	maxlen = GetMaxTraceLengthToFetch();

	std::cerr << "Allocating memory of length ";
	if(maxlen<1e3) {
		std::cerr << maxlen;
	} else if(maxlen<5e5) {
		std::cerr << maxlen*1e-3f << "k";
	} else if(maxlen<5e8) {
		std::cerr << maxlen*1e-6f << "M";
	} else {
		std::cerr << maxlen*1e-9f << "G";
	}
	std::cerr << " ... ";

	try {
		for(i=0; i<GetNumberOfChannels(); i++) {
			if(GetChannel(i)->IsEnabled()) {
				if(data_allocated[i]) {
					if(data_length[i] == maxlen) {
						// no need to do anything; data is already allocated and of the proper size
						// however it is still weird to call this function
						std::cerr << "Warning: Memory for channel " << (char)('A'+i) << " has already been allocated.\n";
					} else {
						std::cerr << "Warning: Memory for channel " << (char)('A'+i) << " has already been allocated; changing size.\n";
						delete [] data[i];
						FILE_LOG(logDEBUG4) << "Measurement::AllocateMemoryBlock - data[" << i << "]" << maxlen;
						data[i] = new short[maxlen];
						data_allocated[i] = true;
						data_length[i] = maxlen;
					}
				} else {
					FILE_LOG(logDEBUG4) << "Measurement::AllocateMemoryBlock - data[" << i << "]" << maxlen;
					
					data[i] = new short[maxlen];
					data_allocated[i] = true;
					data_length[i] = maxlen;
				}
			}
		}
		FILE_LOG(logDEBUG4) << "!!!!!!!!!Measurement::AllocateMemoryBlock - maxlen=" << maxlen;
		std::cerr << "OK\n";
	} catch(...) {
		std::cerr << "Unable to allocate memory in Measurement::AllocateMemoryBlock, tried to allocate " << bytes << "bytes." << std::endl;
		throw;
	}
}

void Measurement::AllocateMemoryRapidBlock(unsigned long bytes)
{
	FILE_LOG(logDEBUG3) << "Measurement::AllocateMemoryRapidBlock (bytes=" << bytes << ")";

	int i;
	unsigned long maxtraces, maxlen, memlen;

	SetMaxMemoryConsumption(bytes);
	maxtraces = GetMaxTracesToFetch();
	// TODO: one should not multiply with GetNumberOfEnabledChannels() !!!
	maxlen    = maxtraces*GetLength();
	memlen    = maxlen*sizeof(short)*GetNumberOfEnabledChannels();

	std::cerr << "Allocating memory of length ";
	if(memlen<1e3) {
		std::cerr << memlen;
	} else if(memlen<5e5) {
		std::cerr << memlen*1e-3f << "k";
	} else if(memlen<5e8) {
		std::cerr << memlen*1e-6f << "M";
	} else {
		std::cerr << memlen*1e-9f << "G";
	}
	std::cerr << " ... ";

	try {
		for(i=0; i<GetNumberOfChannels(); i++) {
			if(GetChannel(i)->IsEnabled()) {
				if(data_allocated[i]) {
					if(data_length[i] == maxlen) {
						// no need to do anything; data is already allocated and of the proper size
						// however it is still weird to call this function
						std::cerr << "Warning: Memory for channel " << (char)('A'+i) << " has already been allocated.\n";
					} else {
						std::cerr << "Warning: Memory for channel " << (char)('A'+i) << " has already been allocated; changing size.\n";
						delete [] data[i];
						// std::cerr << "allocate channel " << i << " with size" << maxlen << "\n";
						data[i] = new short[maxlen];
						data_allocated[i] = true;
						data_length[i] = maxlen;
					}
				} else {
					std::cerr << "(allocate channel " << (char)(i+'A') << " with size " << maxlen << ")\n";
					data[i] = new short[maxlen];
					data_allocated[i] = true;
					data_length[i] = maxlen;
				}
			}
		}
		FILE_LOG(logDEBUG4) << "!!!!!!!!!Measurement::AllocateMemoryBlock - maxlen=" << maxlen;
		std::cerr << "OK\n";
	} catch(...) {
		std::cerr << "Unable to allocate memory in Measurement::AllocateMemoryBlock, tried to allocate " << bytes << "bytes." << std::endl;
		throw;
	}
}

int Measurement::GetNumberOfChannels() const
{
	// FILE_LOG(logDEBUG3) << "Measurement::GetNumberOfChannels";

	return GetPicoscope()->GetNumberOfChannels();
}

int Measurement::GetNumberOfEnabledChannels()
{
	FILE_LOG(logDEBUG3) << "Measurement::GetNumberOfEnabledChannels";

	int i, enabled = 0;

	for(i=0; i<4; i++) {
		if(GetChannel(i)->IsEnabled()) {
			enabled++;
		}
	}

	FILE_LOG(logDEBUG4) << "Measurement::GetNumberOfEnabledChannels - " << enabled << " (A=" << GetChannel(0)->IsEnabled() << ",B=" << GetChannel(1)->IsEnabled() << ",C=" << GetChannel(2)->IsEnabled() << ",D=" << GetChannel(3)->IsEnabled() << ")";

	return enabled;
}

Channel* Measurement::GetChannel(int ch)
{
	// FILE_LOG(logDEBUG3) << "Measurement::GetChannel (channel=" << ch << ")";

	int n = GetNumberOfChannels();
	if(ch>=0 && ch<n) {
		// FILE_LOG(logDEBUG4) << "Measurement::GetChannel -> channel " << (char)('A'+ch) << " - " << channels[ch] << ", index " << channels[ch]->GetIndex();
		return channels[ch];
	} else {
		char message[200];
		sprintf(message, "Picoscope has only channels 0-%d. You cannot request channel with number %d",
		                 GetNumberOfChannels(), ch);
		throw(message);
		return NULL;
	}
}

void Measurement::RunBlock()
{
	FILE_LOG(logDEBUG3) << "Measurement::RunBlock";

	int i;
	Timing t;
	uint32_t max_length=0;

	// we will have to start reading our data from beginning again
	SetNextIndex(0);
	// test if channel settings have already been passed to picoscope
	// and only pass them again if that isn't the case
	FILE_LOG(logDEBUG4) << "Measurement::RunBlock - we have " << GetNumberOfChannels() << " channels";
	for(i=0; i<GetNumberOfChannels(); i++) {
		FILE_LOG(logDEBUG4) << "Measurement::RunBlock - setting channel " << (char)('A'+i) << " (which holds index " << GetChannel(i)->GetIndex() << ")";
		GetChannel(i)->SetChannelInPicoscope();
	}
	// this fixes the timebase if more than a single channel is selected
	FixTimebase();
	// timebase
	SetTimebaseInPicoscope();
	// trigger
	if(IsTriggered()) {
		GetTrigger()->SetTriggerInPicoscope();
	}

	// for rapid block mode
	if(GetNTraces() > 1) {
		// TODO - check that GetLength()*GetNumberOfEnabledChannels()*GetNTraces() doesn't exceed the limit
		if(GetSeries() == PICO_4000) {
			FILE_LOG(logDEBUG2) << "ps4000SetNoOfCaptures(handle=" << GetHandle() << ", nCaptures=" << GetNTraces() << ")";
			GetPicoscope()->SetStatus(ps4000SetNoOfCaptures(
				GetHandle(),    // handle
				GetNTraces())); // nCaptures
		} else {
			FILE_LOG(logDEBUG2) << "ps6000SetNoOfCaptures(handle=" << GetHandle() << ", nCaptures=" << GetNTraces() << ")";
			GetPicoscope()->SetStatus(ps6000SetNoOfCaptures(
				GetHandle(),    // handle
				GetNTraces())); // nCaptures
		}
		if(GetPicoscope()->GetStatus() != PICO_OK) {
			std::cerr << "Unable to set number of captures to " << GetNTraces() << std::endl;
			throw Picoscope::PicoscopeException(GetPicoscope()->GetStatus());
		}
		if(GetSeries() == PICO_4000) {
			FILE_LOG(logDEBUG2) << "ps4000MemorySegments(handle=" << GetHandle() << ", nSegments=" << GetNTraces() << ", &max_length=" << max_length << ")";
			GetPicoscope()->SetStatus(ps4000MemorySegments(
				GetHandle(),   // handle
				GetNTraces(),  // nSegments
				&max_length));
			FILE_LOG(logDEBUG2) << "->ps4000MemorySegments(... max_length=" << max_length << ")";
		} else {
			FILE_LOG(logDEBUG2) << "ps6000MemorySegments(handle=" << GetHandle() << ", nSegments=" << GetNTraces() << ", &max_length=" << max_length << ")";
			GetPicoscope()->SetStatus(ps6000MemorySegments(
				GetHandle(),   // handle
				GetNTraces(),  // nSegments
				&max_length));
			FILE_LOG(logDEBUG2) << "->ps6000MemorySegments(... max_length=" << max_length << ")";
		}
		if(GetPicoscope()->GetStatus() != PICO_OK) {
			std::cerr << "Unable to set number of segments to " << GetNTraces() << std::endl;
			throw Picoscope::PicoscopeException(GetPicoscope()->GetStatus());
		}
		if(max_length < GetLength()) { // TODO: times number of enabled channels
			std::cerr << "The maximum length of trace you can get with " << GetNTraces()
			          << " traces is " << max_length << ", but you requested " << GetLength() << "\n";
			throw;
		}
		if(GetSeries() == PICO_4000) {
			FILE_LOG(logDEBUG2) << "ps4000SetNoOfCaptures(handle=" << GetHandle() << ", nCaptures=" << GetNTraces() << ")";
			GetPicoscope()->SetStatus(ps4000SetNoOfCaptures(
				GetHandle(),    // handle
				GetNTraces())); // nCaptures
		} else {
			FILE_LOG(logDEBUG2) << "ps6000SetNoOfCaptures(handle=" << GetHandle() << ", nCaptures=" << GetNTraces() << ")";
			GetPicoscope()->SetStatus(ps6000SetNoOfCaptures(
				GetHandle(),    // handle
				GetNTraces())); // nCaptures
		}
		if(GetPicoscope()->GetStatus() != PICO_OK) {
			std::cerr << "Unable to set number of captures to " << GetNTraces() << std::endl;
			throw Picoscope::PicoscopeException(GetPicoscope()->GetStatus());
		}
	}

	//std::cerr << "\nPress a key to start fetching the data ...\n";
	//_getch();

	t.Start();
	GetPicoscope()->SetReady(false);
	if(GetSeries() == PICO_4000) {
		FILE_LOG(logDEBUG2) << "ps4000RunBlock(handle=" << GetHandle() << ", noOfPreTriggerSamples=" << GetLengthBeforeTrigger() << ", noOfPostTriggerSamples=" << GetLengthAfterTrigger() << ", timebase=" << timebase << ", oversample=1, *timeIndisposedMs=NULL, segmentIndex=0, lpReady=CallBackBlock, *pParameter=NULL)";
		GetPicoscope()->SetStatus(ps4000RunBlock(
			GetHandle(),              // handle
			GetLengthBeforeTrigger(), // noOfPreTriggerSamples
			GetLengthAfterTrigger(),  // noOfPostTriggerSamples
			timebase,                 // timebase
			1,                        // ovesample
			NULL,                     // *timeIndisposedMs
			0,                        // segmentIndex
			CallBackBlock,            // lpReady
			NULL));                   // *pParameter
	} else {
		FILE_LOG(logDEBUG2) << "ps6000RunBlock(handle=" << GetHandle() << ", noOfPreTriggerSamples=" << GetLengthBeforeTrigger() << ", noOfPostTriggerSamples=" << GetLengthAfterTrigger() << ", timebase=" << timebase << ", oversample=1, *timeIndisposedMs=NULL, segmentIndex=0, lpReady=CallBackBlock, *pParameter=NULL)";
		GetPicoscope()->SetStatus(ps6000RunBlock(
			GetHandle(),              // handle
			GetLengthBeforeTrigger(), // noOfPreTriggerSamples
			GetLengthAfterTrigger(),  // noOfPostTriggerSamples
			timebase,                 // timebase
			1,                        // ovesample
			NULL,                     // *timeIndisposedMs
			0,                        // segmentIndex
			CallBackBlock,            // lpReady
			NULL));                   // *pParameter
	}
	if(GetPicoscope()->GetStatus() != PICO_OK) {
		std::cerr << "Unable to start collecting samples" << std::endl;
		throw Picoscope::PicoscopeException(GetPicoscope()->GetStatus());
	} else {
		std::cerr << "Start collecting samples in "
		          << ((GetNTraces() > 1) ? "rapid " : "") << "block mode ... ";
	}
	// TODO: maybe we want it to be asynchronous
	// TODO: catch the _kbhit event!!!
	// while (!Picoscope::IsReady() && !_kbhit()) {
	while (!Picoscope::IsReady()) {
		Sleep(200);
	}
	t.Stop();
	std::cerr << "OK (" << t.GetSecondsDouble() << "s)\n";

	// sets the index from where we want to start reading data to zero
	SetNextIndex(0UL);
}

// TODO: we might want to use multiple buffers at the same time
// returns the length of data
// TODO: the first part only needs to be called once; so we should move the code at the end of RunBlock
//       unless we want to alternate between allocated memory (to enable parallel readouts)
unsigned long Measurement::GetNextData()
{
	FILE_LOG(logDEBUG3) << "Measurement::GetNextData";

	int i;
	short overflow=0;
	uint32_t length_of_trace_askedfor, length_of_trace_fetched;
	Timing t;

	// it makes no sense to read any further: we are already at the end
	if(GetNextIndex() >= GetLength()) {
		std::cerr << "Stop fetching data from osciloscope." << std::endl;
		return 0UL;
	}

	length_of_trace_askedfor = GetMaxTraceLengthToFetch();
	if(GetNextIndex() + length_of_trace_askedfor > GetLength()) {
		length_of_trace_askedfor = GetLength() - GetNextIndex();
	}
	// allocate buffers
	for(i=0; i<GetNumberOfChannels(); i++) {
		if(GetChannel(i)->IsEnabled()) {
			if(data_allocated[i] == false) {
				throw "Unable to get data. Memory is not allocated.";
			}
			if(GetSeries() == PICO_4000) {
				FILE_LOG(logDEBUG2) << "ps4000SetDataBuffer(handle=" << GetHandle() << ", channel=" << i << ", *buffer=<data[i]>, bufferLength=" << GetMaxTraceLengthToFetch() << ")";
				GetPicoscope()->SetStatus(ps4000SetDataBuffer(
					GetHandle(),                  // handle
					(PS4000_CHANNEL)i,            // channel
					data[i],                      // *buffer
					GetMaxTraceLengthToFetch())); // bufferLength
			} else {
				FILE_LOG(logDEBUG2) << "ps6000SetDataBuffer(handle=" << GetHandle() << ", channel=" << i << ", *buffer=<data[i]>, bufferLength=" << GetMaxTraceLengthToFetch() << ", downSampleRatioMode=PS6000_RATIO_MODE_NONE)";
				GetPicoscope()->SetStatus(ps6000SetDataBuffer(
					GetHandle(),                // handle
					(PS6000_CHANNEL)i,          // channel
					data[i],                    // *buffer
					GetMaxTraceLengthToFetch(), // bufferLength
					PS6000_RATIO_MODE_NONE));   // downSampleRatioMode
			}
			if(GetPicoscope()->GetStatus() != PICO_OK) {
				std::cerr << "Unable to set memory for channel." << std::endl;
				throw Picoscope::PicoscopeException(GetPicoscope()->GetStatus());
			}
		}
	}
	// fetch data
	length_of_trace_fetched = length_of_trace_askedfor;
	std::cerr << "Get data for points " << GetNextIndex() << "-" << GetNextIndex()+length_of_trace_askedfor << " (" << 100.0*(GetNextIndex()+length_of_trace_askedfor)/GetLength() << "%) ... ";
	t.Start();
	// std::cerr << "length of buffer: " << data_length[0] << ", length of requested trace: " << length_of_trace_askedfor << " ... ";
	if(GetSeries() == PICO_4000) {
		FILE_LOG(logDEBUG2) << "ps4000GetValues(handle=" << GetHandle() << ", startIndex=" << GetNextIndex() << ", *noOfSamples=" << length_of_trace_fetched << ", downSampleRatio=1, downSampleRatioMode=PS4000_RATIO_MODE_NONE, segmentIndex=0, *overflow)";
		GetPicoscope()->SetStatus(ps4000GetValues(
			GetHandle(),                // handle
			// TODO: start index
			GetNextIndex(),             // startIndex
			// this could also be min(GetMaxTraceLengthToFetch(),wholeLength-startindex)
			&length_of_trace_fetched,   // *noOfSamples
			1,                          // downSampleRatio
			PS4000_RATIO_MODE_NONE,     // downSampleRatioMode
			0,                          // segmentIndex
			&overflow));                // *overflow
		FILE_LOG(logDEBUG2) << "-> length_of_trace_fetched=" << length_of_trace_fetched << "\n";
	} else {
		FILE_LOG(logDEBUG2) << "ps6000GetValues(handle=" << GetHandle() << ", startIndex=" << GetNextIndex() << ", *noOfSamples=" << length_of_trace_fetched << ", downSampleRatio=1, downSampleRatioMode=PS6000_RATIO_MODE_NONE, segmentIndex=0, *overflow)";
		GetPicoscope()->SetStatus(ps6000GetValues(
			GetHandle(),                // handle
			// TODO: start index
			GetNextIndex(),             // startIndex
			// this could also be min(GetMaxTraceLengthToFetch(),wholeLength-startindex)
			&length_of_trace_fetched,   // *noOfSamples
			1,                          // downSampleRatio
			PS6000_RATIO_MODE_NONE,     // downSampleRatioMode
			0,                          // segmentIndex
			&overflow));                // *overflow
		FILE_LOG(logDEBUG2) << "-> length_of_trace_fetched=" << length_of_trace_fetched << "\n";
	}
	t.Stop();
	if(GetPicoscope()->GetStatus() != PICO_OK) {
		std::cerr << "Unable to set memory for channel." << std::endl;
		throw Picoscope::PicoscopeException(GetPicoscope()->GetStatus());
	}
	if(overflow) {
		for(i=0; i<GetNumberOfChannels(); i++) {
			if(overflow & (1<<i)) {
				std::cerr << "Warning: Overflow on channel " << (char)('A'+i) << ".\n";
			}
		}
	}
	if(length_of_trace_fetched != length_of_trace_askedfor) {
		std::cerr << "Warning: The number of read samples was smaller than requested.\n";
	}
	std::cerr << "OK ("<< t.GetSecondsDouble() <<"s)\n";
	SetLengthFetched(length_of_trace_fetched);
	SetNextIndex(GetNextIndex()+length_of_trace_fetched);

	return length_of_trace_fetched;
}

unsigned long Measurement::GetNextDataBulk()
{
	FILE_LOG(logDEBUG3) << "Measurement::GetNextDataBulk";

	unsigned long i, j, index;
	short *overflow;
	uint32_t traces_asked_for, length_of_trace_fetched;
	// unsigned long length_of_trace_askedfor, length_of_trace_fetched;
	Timing t;

	// std::cerr << "(GetNextDataBulk: " << GetNextIndex() << ", " << GetMaxTracesToFetch() << ")\n";

	// it makes no sense to read any further: we are already at the end
	if(GetNextIndex() >= GetNTraces()) {
		std::cerr << "Stop fetching data from ociloscope." << std::endl;
		return 0UL;
	}

/*
	try {
		for(i=0; i<GetNumberOfChannels(); i++) {
			if(GetChannel(i)->IsEnabled()) {
				delete [] data[i];
				data[i] = new short[GetMaxTracesToFetch()*GetLength()];
			}
		}
	} catch(...) {
		std::cerr << "Unable to allocate memory in Measurement::AllocateMemoryBlock." << std::endl;
		throw;
	}
/**/

	traces_asked_for = GetMaxTracesToFetch();
	if(GetNextIndex() + traces_asked_for > GetNTraces()) {
		traces_asked_for = GetNTraces() - GetNextIndex();
	}
	// allocate buffers
	for(i=0; i<GetNumberOfChannels(); i++) {
		if(GetChannel(i)->IsEnabled()) {
			FILE_LOG(logDEBUG4) << "Measurement::GetNextDataBulk - memset data[i]" << GetLength()*traces_asked_for*sizeof(short);
			memset(data[i], 0, GetLength()*traces_asked_for*sizeof(short));
			FILE_LOG(logDEBUG4) << "done";
		}
		for(j=0; j<traces_asked_for; j++) {
			index = j+GetNextIndex();
			if(GetChannel(i)->IsEnabled()) {
				if(data_allocated[i] == false) {
					throw "Unable to get data. Memory is not allocated.";
				}
				if(GetSeries() == PICO_4000) {
					// GetPicoscope()->SetStatus(ps4000SetDataBufferBulk(
					// 	GetHandle(),                  // handle
					// 	(PS4000_CHANNEL)i,            // channel
					// 	data[i],                      // *buffer
					// 	GetMaxTraceLengthToFetch())); // bufferLength
					GetPicoscope()->SetStatus(ps4000SetDataBufferBulk(
						GetHandle(),                // handle
						(PS4000_CHANNEL)i,          // channel
						&data[i][j*GetLength()],    // *buffer
						GetLength(),                // bufferLength
						index));                    // waveform
				} else {
					// unsigned long dj = (j+GetNextIndex()/GetMaxTracesToFetch())%traces_asked_for;
					// std::cerr << "-- (set data buffer bulk: [" << i << "][" << dj
					// 	<< "], len:" << GetLength() << ", index:" << index
					// 	<< ", from:" << GetNextIndex()
					// 	<< ", to:" << GetNextIndex()+traces_asked_for-1 << "\n";
					GetPicoscope()->SetStatus(ps6000SetDataBufferBulk(
						GetHandle(),                // handle
						(PS6000_CHANNEL)i,          // channel
						&data[i][j*GetLength()],    // *buffer
						GetLength(),                // bufferLength
						index,                      // waveform
						PS6000_RATIO_MODE_NONE));   // downSampleRatioMode
				}
				if(GetPicoscope()->GetStatus() != PICO_OK) {
					std::cerr << "Unable to set memory for channel." << std::endl;
					throw Picoscope::PicoscopeException(GetPicoscope()->GetStatus());
				}
			}
		}
	}
	// fetch data
	// length_of_trace_fetched = length_of_trace_askedfor;
	std::cerr << "Get data for traces " << GetNextIndex() << "-" << GetNextIndex()+traces_asked_for << " (" << 100.0*(GetNextIndex()+traces_asked_for)/GetNTraces() << "%) ... ";
	overflow = new short[traces_asked_for];
	memset(overflow, 0, traces_asked_for*sizeof(short));
	// std::cerr << "-- x1\n";
	t.Start();
	// std::cerr << "length of buffer: " << data_length[0] << ", length of requested trace: " << length_of_trace_askedfor << " ... ";
	if(GetSeries() == PICO_4000) {
		length_of_trace_fetched = GetLength();
		GetPicoscope()->SetStatus(ps4000GetValuesBulk(
			GetHandle(),                // handle
			&length_of_trace_fetched,   // *noOfSamples
			// TODO: start index
			GetNextIndex(),             // fromSegmentIndex
			GetNextIndex()+traces_asked_for-1, // toSegmentIndex
			// this could also be min(GetMaxTraceLengthToFetch(),wholeLength-startindex)
			overflow));                // *overflow
	} else {
		// TODO: not sure about this ...
		length_of_trace_fetched = GetLength();
		GetPicoscope()->SetStatus(ps6000GetValuesBulk(
			GetHandle(),                // handle
			&length_of_trace_fetched,   // *noOfSamples
			// TODO: start index
			GetNextIndex(),             // fromSegmentIndex
			GetNextIndex()+traces_asked_for-1, // toSegmentIndex
			// this could also be min(GetMaxTraceLengthToFetch(),wholeLength-startindex)
			1,                          // downSampleRatio
			PS6000_RATIO_MODE_NONE,     // downSampleRatioMode
			overflow));                // *overflow
	}
	t.Stop();
	// std::cerr << "-- x2\n";
	if(GetPicoscope()->GetStatus() != PICO_OK) {
		std::cerr << "Unable to set memory for channel." << std::endl;
		throw Picoscope::PicoscopeException(GetPicoscope()->GetStatus());
	}
	for(i=0; i<traces_asked_for; i++) {
		for(j=0; i<GetNumberOfChannels(); i++) {
			if(overflow[i] & (1<<j)) {
				FILE_LOG(logWARNING) << "Warning: Overflow on channel " << (char)('A'+j) << " of trace " << i+GetNextIndex() << ".\n";
			}
		}
	}
	delete [] overflow;
	// if(length_of_trace_fetched != length_of_trace_askedfor) {
	// 	std::cerr << "Warning: The number of read samples was smaller than requested.\n";
	// }

	// getting timestamps
	int64_t *timestamps;
	PS6000_TIME_UNITS *timeunits;

	timestamps = new int64_t[traces_asked_for];
	timeunits  = new PS6000_TIME_UNITS[traces_asked_for];

	if(GetSeries() == PICO_4000) {
		// NOT YET IMPLEMENTED
	} else {
		FILE_LOG(logDEBUG2) << "ps6000GetValuesTriggerTimeOffsetBulk64(handle=" << GetHandle() << ", *timestamps, *timeunits, fromSegmentIndex=" << GetNextIndex() << ", toSegmentIndex=" << GetNextIndex()+traces_asked_for-1 << ")";
		GetPicoscope()->SetStatus(ps6000GetValuesTriggerTimeOffsetBulk64(
			GetHandle(),                // handle
			timestamps,                 // *times
			timeunits,                  // *timeUnits
			GetNextIndex(),                      // fromSegmentIndex
			GetNextIndex()+traces_asked_for-1)); // toSegmentIndex
	}
	if(GetPicoscope()->GetStatus() != PICO_OK) {
		std::cerr << "Unable to get timestamps." << std::endl;
		throw Picoscope::PicoscopeException(GetPicoscope()->GetStatus());
	}

	// for(i=0; i<traces_asked_for; i++) {
	// 	std::cerr << "time " << i << ": " << timestamps[i] << "\n";
	// }
	SetRate(traces_asked_for, timestamps[0], timeunits[0], timestamps[traces_asked_for-1], timeunits[traces_asked_for-1]);
	// if(timeunits[0] != timeunits[traces_asked_for-1]) {
	// 	FILE_LOG(logWARNING) << "time unit of the first and last sample differ; rate is not reliable; TIMING seems to be broken anyway";
	// }

	delete [] timestamps;
	delete [] timeunits;


	std::cerr << "OK ("<< t.GetSecondsDouble() <<"s)\n";

	// std::cerr << "length of trace fetched: " << length_of_trace_fetched << "\n";
	// std::cerr << "total length: " << traces_asked_for*length_of_trace_fetched << "\n";

	SetLengthFetched(traces_asked_for*length_of_trace_fetched);
	// std::cerr << "-- next index is now " << GetNextIndex() << ", will be set to " << GetNextIndex() + traces_asked_for << "\n";
	SetNextIndex(GetNextIndex()+traces_asked_for);
	// std::cerr << "-- next index is now " << GetNextIndex() << "\n";
	// std::cerr << "-- return value " << traces_asked_for << "\n";

	return traces_asked_for;
}

void Measurement::SetLengthFetched(unsigned long l)
{
	FILE_LOG(logDEBUG3) << "Measurement::SetLengthFetched (length=" << l << ")";

	length_fetched = l;
}

// TODO: we might want to use multiple buffers at the same time
void Measurement::WriteDataBin(FILE *f, int channel)
{
	Timing t;
	long size_written;
	int i, j;

	const unsigned int length_datachunk = 1000000;
	char data_8bit[1000000];
	// std::vector<char>data_8bit(1000);

	std::cerr << "Write binary data for channel " << (char)('A'+channel) << " ... ";
	t.Start();
	// TODO: test if file exists
	if(channel < 0 || channel >= GetNumberOfChannels()) {
		throw "You can only write data for channels 0 - (N-1).";
	} else {
		if(GetChannel(channel)->IsEnabled()) {

			/*
			size_written = fwrite(data[channel], sizeof(short), GetLengthFetched(), f);
			// make sure the data is written
			fflush(f);
			if(size_written < GetLengthFetched()) {
				FILE_LOG(logERROR) << "Measurement::WriteDataBin didn't manage to write to file.";
			}*/
			// TODO: if only 8 bits
			int length_fetched   = GetLengthFetched();
			// int length_datachunk = data_8bit.size();
			for(i=0; i<length_fetched; i+=length_datachunk) {
				if(GetSeries() == PICO_6000) {
					for(j=0; j<length_datachunk && i+j<length_fetched; j++) {
						data_8bit[j] = data[channel][i+j] >> 8;
					}
					size_written = fwrite(data_8bit, sizeof(char), j, f);
				} else {
					// TODO: test!!!
					size_written = fwrite(data[channel]+1, sizeof(data[0][0]), j, f);
				}
				fflush(f);
				if(size_written < j) {
					FILE_LOG(logERROR) << "Measurement::WriteDataBin didn't manage to write to file.";
				}
			}
			// size_written = fwrite(data[channel], sizeof(short), GetLengthFetched(), f);
			// make sure the data is written
			// fflush(f);
			// if(size_written < GetLengthFetched()) {
			// 	FILE_LOG(logERROR) << "Measurement::WriteDataBin didn't manage to write to file.";
			// }
		} else {
			std::cerr << "The requested channel " << (char)('A'+channel) << "is not enabled.\n";
			throw "The requested channel is not enabled.";
		}
	}
	t.Stop();
	std::cerr << "OK ("<< t.GetSecondsDouble() <<"s)\n";
}

void Measurement::WriteDataTxt(FILE *f, int channel)
{
	int i;
	Timing t;

	std::cerr << "Write text data for channel " << (char)('A'+channel) << " ... ";
	t.Start();
	// TODO: test if file exists
	if(channel < 0 || channel >= GetNumberOfChannels()) {
		throw "You can only write data for channels 0 - (N-1).";
	} else {
		if(GetChannel(channel)->IsEnabled()) {
			if(GetSeries() == PICO_6000) {
				for(i=0; i<GetLengthFetched(); i++) {
					fprintf(f, "%d\n", data[channel][i]>>8);
				}
			} else {
				for(i=0; i<GetLengthFetched(); i++) {
					fprintf(f, "%d\n", data[channel][i]);
				}
			}
			// make sure the data is written
			fflush(f);
		} else {
			std::cerr << "The requested channel " << (char)('A'+channel) << "is not enabled.\n";
			throw "The requested channel is not enabled.";
		}
	}
	t.Stop();
	std::cerr << "OK ("<< t.GetSecondsDouble() <<"s)\n";
}

void Measurement::SetNextIndex(unsigned long index)
{
	FILE_LOG(logDEBUG3) << "Measurement::SetNextIndex (index=" << index << ")";

	// TODO: check that the index makes sense at all
	next_index = index;
}

void Measurement::AddSimpleTrigger(Channel* ch, double x_frac, double y_frac)
{
	FILE_LOG(logDEBUG3) << "Measurement::AddSimpleTrigger (channel=" << ch << ", x_frac=" << x_frac << ", y_frac=" << y_frac << ")";

	is_triggered = true;
	// TODO: must be improved for more fancy triggers
//	if(trigger != NULL) delete trigger;
	trigger = new Trigger(ch, x_frac, y_frac);
}

void Measurement::SetTrigger(Trigger *tr)
{
	// TODO: print out trigger
	FILE_LOG(logDEBUG3) << "Measurement::SetTrigger (trigger=" << tr << ")";
	FILE_LOG(logDEBUG4) << "Measurement::SetTrigger - previous values: trigger=" << trigger << ", is_triggered=" << is_triggered;

	is_triggered = true;
	// TODO: must be improved for more fancy triggers
	if(trigger != NULL) delete trigger;
	trigger = tr;
	FILE_LOG(logDEBUG4) << "Measurement::SetTrigger - new values: trigger=" << trigger << ", is_triggered=" << is_triggered;
}

void Measurement::InitializeSignalGenerator()
{
	FILE_LOG(logDEBUG3) << "Measurement::InitializeSignalGenerator";

	float frequency = signal_generator_frequency;

	if(use_signal_generator == false) {
		FILE_LOG(logDEBUG4) << "Measurement::InitializeSignalGenerator - skipped, no need for generator";
		return;
	}

	FILE_LOG(logDEBUG4) << "Measurement::InitializeSignalGenerator";
	if(frequency < PS6000_MIN_FREQUENCY || frequency > PS6000_SQUARE_MAX_FREQUENCY) {
		std::cerr << "Frequency of signal generator (" << frequency << " Hz) is not valid" << std::endl;
		throw;
	}
	if(GetSeries() == PICO_4000) {
		throw("Not yet implemented: ps4000SetSigGenBuiltIn.\n");
		// TODO
	} else {
		// throw("not yet implemented.\n");
		GetPicoscope()->SetStatus(ps6000SetSigGenBuiltIn(
			GetHandle(),                // handle
			0,                          // offsetVoltage
			signal_generator_peak_to_peak_in_microvolts,
			PS6000_SQUARE,
			frequency,
			frequency,
			0.0,
			1000.0,
			PS6000_UP,
			PS6000_ES_OFF,
			0,
			0,
			PS6000_SIGGEN_RISING,
			PS6000_SIGGEN_NONE,
			0));
	}
	if(GetPicoscope()->GetStatus() != PICO_OK) {
		std::cerr << "Unable to setup signal generator." << std::endl;
		throw Picoscope::PicoscopeException(GetPicoscope()->GetStatus());
	}
}

void Measurement::AddSignalGeneratorSquare(unsigned long peak_to_peak_in_microvolts, float frequency)
{
	FILE_LOG(logDEBUG3) << "Measurement::AddSignalGeneratorSquare (peak_to_peak_in_microvolts=" << peak_to_peak_in_microvolts << ", frequency=" << frequency << ")";

	use_signal_generator = true;

	signal_generator_peak_to_peak_in_microvolts = peak_to_peak_in_microvolts;
	signal_generator_frequency = frequency;
}

// TODO: get rid of dependency on PS6000_TIME_UNITS in declaration
// implementation may change
void Measurement::SetRate(long n_events, int64_t t1, PS6000_TIME_UNITS time_unit1, int64_t t2, PS6000_TIME_UNITS time_unit2)
{
	FILE_LOG(logDEBUG3) << "Measurement::SetRate (n_events=" << n_events << ", ...)";
	if(n_events < 2) {
		rate_per_second = 0.0;
		return;
	}

	double tt1 = t1, tt2 = t2;

	// rate_per_second = (double)(n_events-1)/(double)dt;

	switch(time_unit1) {
		case PS6000_FS: tt1 *= 1e-15; FILE_LOG(logDEBUG4) << "t1=" << t1 << "fs"; break;
		case PS6000_PS: tt1 *= 1e-12; FILE_LOG(logDEBUG4) << "t1=" << t1 << "ps"; break;
		case PS6000_NS: tt1 *= 1e-9;  FILE_LOG(logDEBUG4) << "t1=" << t1 << "ns"; break;
		case PS6000_US: tt1 *= 1e-6;  FILE_LOG(logDEBUG4) << "t1=" << t1 << "us"; break;
		case PS6000_MS: tt1 *= 1e-3;  FILE_LOG(logDEBUG4) << "t1=" << t1 << "ms"; break;
		case PS6000_S : tt1 *= 1;     FILE_LOG(logDEBUG4) << "t1=" << t1 << "s "; break;
		default: FILE_LOG(logDEBUG4) << "t1=weird "; break;
	};

	switch(time_unit2) {
		case PS6000_FS: tt2 *= 1e-15; FILE_LOG(logDEBUG4) << "t2=" << t2 << "fs"; break;
		case PS6000_PS: tt2 *= 1e-12; FILE_LOG(logDEBUG4) << "t2=" << t2 << "ps"; break;
		case PS6000_NS: tt2 *= 1e-9;  FILE_LOG(logDEBUG4) << "t2=" << t2 << "ns"; break;
		case PS6000_US: tt2 *= 1e-6;  FILE_LOG(logDEBUG4) << "t2=" << t2 << "us"; break;
		case PS6000_MS: tt2 *= 1e-3;  FILE_LOG(logDEBUG4) << "t2=" << t2 << "ms"; break;
		case PS6000_S : tt2 *= 1;     FILE_LOG(logDEBUG4) << "t2=" << t2 << "s "; break;
		default: FILE_LOG(logDEBUG4) << "t2=weird (" << t2 << ") "; break;
	};

	rate_per_second = (n_events-1)/(tt2 - tt1);
}

double Measurement::GetRatePerSecond()
{
	FILE_LOG(logDEBUG3) << "Measurement::GetRatePerSecond()";
	return rate_per_second;
}
