/* A work-in-progess MEGA65 (Commodore 65 clone origins) emulator
   Part of the Xemu project, please visit: https://github.com/lgblgblgb/xemu
   Copyright (C)2016-2021 LGB (Gábor Lénárt) <lgblgblgb@gmail.com>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */

#include "xemu/emutools.h"
#include "xemu/sid.h"
#include "xemu/opl3.h"
#include "audio65.h"
// For D7XX (audio DMA):
#include "io_mapper.h"
// For accessing memory (audio DMA):
#include "memory_mapper.h"


SDL_AudioDeviceID audio = 0;	// SDL audio device

int stereo_separation = AUDIO_DEFAULT_SEPARATION;
int audio_volume      = AUDIO_DEFAULT_VOLUME;

static int stereo_separation_orig = 100;
static int stereo_separation_other = 0;
struct SidEmulation sid[4];
static int mixing_freq;		// playback sample rate (in Hz) of the emulator itself
static double dma_audio_mixing_value;
static opl3_chip opl3;



void audio65_opl3_write ( Uint8 reg, Uint8 data )
{
	//OPL3_WriteReg(&opl3, reg, data);
	OPL3_WriteRegBuffered(&opl3, reg, data);
}


#ifdef AUDIO_EMULATION
static inline void render_dma_audio ( int channel, short *buffer, int len )
{
	static short sample[4];	// current sample values of the four channels, normalized to 16 bit signed value
	static double rate_counter[4] = {0,0,0,0};
	Uint8 *chio = D7XX + 0x20 + channel * 0x10;
	unsigned int addr = chio[0xA] + (chio[0xB] << 8) + (chio[0xC] << 16);
	const Uint16 limit = chio[0x7] + (chio[0x8] << 8);
	const double rate_step =
		(double)(chio[4] + (chio[5] << 8) + (chio[6] << 16))		// this value is added to the 24 bit counter every 40.5MHz clock, to see overflow
		*
		dma_audio_mixing_value;
	for (unsigned int i = 0; i < len; i++) {
		if (!(chio[0] & 0x80) || (chio[0] & 0x08)) {
			// silence
			sample[channel] = 0;
			rate_counter[channel] = 0;
			buffer[i] = 0;
			continue;
		}
		rate_counter[channel] += rate_step;
		// the reason for while loop: real MEGA65 would not do this, but mixing frequency of audio on typical
		// PC is low (~44-48KHz) compared to the native MEGA65 40.5MHz clock. Thus in some cases we need more steps
		// within a single point. Surely it can be optimized a better way, but for now ...
		while (rate_counter[channel] >= 0x1000000) {
			rate_counter[channel] -= 0x1000000;
			if (XEMU_UNLIKELY(addr >= (sizeof(main_ram) - 1))) {	// do not overflow fast RAM, as DMA audio can do 24 bit, but fast RAM is much smaller
				sample[channel] = 0;
				addr += ((chio[0] & 3) == 3) ? 2 : 1;
			} else {
				Uint16 unsigned_read;
				switch (chio[0] & 3) {
					case 0:	// lower nybble only
						unsigned_read = (main_ram[addr] & 0x0F) << 12;
						addr++;
						break;
					case 1:	// high nybble only
						unsigned_read = (main_ram[addr] & 0xF0) << 8;
						addr++;
						break;
					case 2:	// 8 bit sample
						unsigned_read = main_ram[addr] << 8;
						addr++;
						break;
					case 3:	// 16 bit sample
						unsigned_read = main_ram[addr] + (main_ram[addr + 1] << 8);
						addr += 2;
						break;
					default:
						XEMU_UNREACHABLE();
				}
				// TODO: use unsigned_read, convert signed<->unsigned stuff, etc ....
				// NOTE: the read above to 'unsigned_read' can be still signed, we just read as unsigned 16 bit uniform data
				// so we can transform here to the output needs (that is: signed 16 bit). It's based on MEGA65's audio DMA
				// setting: is it fed by unsigned or signed samples?
				sample[channel] = unsigned_read - 0x8000;
				sample[channel] = unsigned_read;
				sample[channel] = (sample[channel] * chio[9]) / 0xFF;	// volume control (reg9, max volume $FF)
			}
			if (XEMU_UNLIKELY((addr & 0xFFFF) == limit)) {
				// if top address is reached: either stop, or loop (on looped samples only!)
				if ((chio[0] & 0x40)) {
					addr = chio[1] + (chio[2] << 8) + (chio[3] << 16);	// looping, set address to the begin address
				} else {
					chio[0] |= 8;		// no loop, stop!
					sample[channel] = 0;
					rate_counter[channel] = 0;
					break;
				}
			}
		}
		// render one sample for this channel to the buffer
		buffer[i] = sample[channel];
	}
	// End of loop:
	// write back address ...
	chio[0xA] =  addr        & 0xFF;
	chio[0xB] = (addr >>  8) & 0xFF;
	chio[0xC] = (addr >> 16) & 0xFF;
}


void audio_set_stereo_parameters ( int vol, int sep )
{
	if (sep == AUDIO_UNCHANGED_SEPARATION) {
		sep = stereo_separation;
	} else {
		if (sep > 100)
			sep = 100;
		else if (sep < -100)
			sep = -100;
		stereo_separation = sep;
	}
	if (vol == AUDIO_UNCHANGED_VOLUME) {
		vol = audio_volume;
	} else {
		if (vol > 100)
			vol = 100;
		else if (vol < 0)
			vol = 0;
		audio_volume = vol;
	}
	//sep = ((sep + 100) * 0x100) / 200;
	sep = (sep + 100) / 2;
	//sep = (sep + 100) * 0x100 / 200;
	stereo_separation_orig  = (sep * vol) / 100;
	stereo_separation_other = ((100 - sep) * vol) / 100;
	DEBUGPRINT("AUDIO: volume is set to %d%%, stereo separation is %d%% [component-A is %d, component-B is %d]" NL, audio_volume, stereo_separation, stereo_separation_orig, stereo_separation_other);
}



static void audio_callback ( void *userdata, Uint8 *stereo_out_stream, int len )
{
#if 1
	//DEBUGPRINT("AUDIO: audio callback, wants %d samples" NL, len);
	len >>= 2;	// the real size if /4, since it's a stereo stream, and 2 bytes/sample, we want to render
	short streams[9][len];	// currently. 4 dma channels + 4 SIDs + 1 for OPL3
	for (int i = 0; i < 4; i++)
		render_dma_audio(i, streams[i], len);
	sid_render(&sid[0], streams[4], len, 1);	// $D400 - left
	sid_render(&sid[1], streams[5], len, 1);	// $D420 - left
	sid_render(&sid[2], streams[6], len, 1);	// $D440 - right
	sid_render(&sid[3], streams[7], len, 1);	// $D460 - right
	OPL3_GenerateStream(&opl3, streams[8], len, 1);
	// Now mix channels
	for (int i = 0; i < len; i++) {
		// mixing streams together
		int orig_left  = (int)streams[0][i] + (int)streams[1][i] + (int)streams[4][i] + (int)streams[5][i] + (int)streams[8][i];
		int orig_right = (int)streams[2][i] + (int)streams[3][i] + (int)streams[6][i] + (int)streams[7][i] + (int)streams[8][i];
#if 1
		// channel stereo separation (including inversion) + volume handling
		int left  = ((orig_left  * stereo_separation_orig) / 100) + ((orig_right * stereo_separation_other) / 100);
		int right = ((orig_right * stereo_separation_orig) / 100) + ((orig_left  * stereo_separation_other) / 100);
#else
		int left = orig_left;
		int right = orig_right;
#endif
		// do some ugly clipping ...
		if      (left  >  0x7FFF) left  =  0x7FFF;
		else if (left  < -0x8000) left  = -0x8000;
		if      (right >  0x7FFF) right =  0x7FFF;
		else if (right < -0x8000) right = -0x8000;
		// write the output stereo stream for SDL (it's an interlaved left-right-left-right kind of thing)
		((short*)stereo_out_stream)[ i << 1     ] = left;
		((short*)stereo_out_stream)[(i << 1) + 1] = right;
	}
#else
	// DEBUG("AUDIO: audio callback, wants %d samples" NL, len);
	// We use the trick, to render boths SIDs with step of 2, with a byte offset
	// to get a stereo stream, wanted by SDL.
	//sid_render(&sid2, ((short *)(stereo_out_stream)) + 0, len >> 1, 2);	// SID @ left
	//sid_render(&sid1, ((short *)(stereo_out_stream)) + 1, len >> 1, 2);	// SID @ right
#endif
}
#endif


void audio65_init ( int sid_cycles_per_sec, int sound_mix_freq, int volume, int separation )
{
	// We always initialize SIDs, even if no audio emulation is compiled in
	// Since there can be problem to write SID registers otherwise?
	sid_init(&sid[0], sid_cycles_per_sec, sound_mix_freq);
	sid_init(&sid[1], sid_cycles_per_sec, sound_mix_freq);
	sid_init(&sid[2], sid_cycles_per_sec, sound_mix_freq);
	sid_init(&sid[3], sid_cycles_per_sec, sound_mix_freq);
	OPL3_Reset(&opl3, sound_mix_freq);
#ifdef AUDIO_EMULATION
	mixing_freq = sound_mix_freq;
	dma_audio_mixing_value =  (double)40500000.0 / (double)sound_mix_freq;	// ... but with Xemu we use a much lower sampling rate, thus compensate (will fail on samples, rate >= xemu_mixing_rate ...)
	SDL_AudioSpec audio_want, audio_got;
	SDL_memset(&audio_want, 0, sizeof(audio_want));
	audio_want.freq = sound_mix_freq;
	audio_want.format = AUDIO_S16SYS;	// used format by SID emulation (ie: signed short)
	audio_want.channels = 2;		// that is: stereo, for the two SIDs
	audio_want.samples = 1024;		// Sample size suggested (?) for the callback to render once
	audio_want.callback = audio_callback;	// Audio render callback function, called periodically by SDL on demand
	audio_want.userdata = NULL;		// Not used, "userdata" parameter passed to the callback by SDL
	audio = SDL_OpenAudioDevice(NULL, 0, &audio_want, &audio_got, 0);
	if (audio) {
		for (int i = 0; i < SDL_GetNumAudioDevices(0); i++)
			DEBUG("AUDIO: audio device is #%d: %s" NL, i, SDL_GetAudioDeviceName(i, 0));
		// Sanity check that we really got the same audio specification we wanted
		if (audio_want.freq != audio_got.freq || audio_want.format != audio_got.format || audio_want.channels != audio_got.channels) {
			SDL_CloseAudioDevice(audio);	// forget audio, if it's not our expected format :(
			audio = 0;
			ERROR_WINDOW("Audio parameter mismatches.");
		}
		DEBUGPRINT("AUDIO: initialized (#%d), %d Hz, %d channels, %d buffer sample size." NL, audio, audio_got.freq, audio_got.channels, audio_got.samples);
	} else
		ERROR_WINDOW("Cannot open audio device!");
	audio_set_stereo_parameters(volume, separation);
#else
	DEBUGPRINT("AUDIO: has been disabled at compilation time." NL);
#endif
}
