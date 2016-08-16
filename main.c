#define _CRT_SECURE_NO_WARNINGS
#include <Windows.h>
#include <stdint.h>
#include <stdio.h>
#include <Shlwapi.h>

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

#define SINGLE_DEVICE 0

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

#define ASSERT(X) ((X)?(void)0:__debugbreak(),(void)0)

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

struct WavFile
{
	void *buffer;
	const WAVEFORMATEX *fmt;
	const void *data;
	uint32_t dataSize;
	WAVEHDR wh;
};
typedef struct WavFile WavFile;

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

#define PLAY_QUEUE_LENGTH (1000)
#define NUM_KEYS (256)

static WavFile g_wavFiles[NUM_KEYS][2];
static __declspec(thread) char g_errorTextBuffer[1000];
static BOOL g_keyStates[NUM_KEYS];
#if SINGLE_DEVICE
static HWAVEOUT g_hWaveOut;
#else
static HWAVEOUT g_hWaveOuts[NUM_KEYS];
#endif
static HHOOK g_hKeyboardHook;

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

static void Fatal(const char *fmt, ...)
{
	char tmp[1000];

	va_list v;
	va_start(v, fmt);
	_vsnprintf(tmp, sizeof tmp, fmt, v);
	tmp[sizeof tmp - 1] = 0;
	va_end(v);

	MessageBox(NULL, tmp, "Oh dear", MB_OK | MB_ICONERROR);

	exit(1);
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

static void dprintf(const char *fmt, ...)
{
	char tmp[15000];

	va_list v;
	va_start(v, fmt);
	_vsnprintf(tmp, sizeof tmp, fmt, v);
	tmp[sizeof tmp - 1] = 0;
	va_end(v);

	OutputDebugString(tmp);
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////


static char *GetErrorText(DWORD error)
{
	FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM, NULL, error, 0,
		g_errorTextBuffer, sizeof g_errorTextBuffer, NULL);

	char *p = g_errorTextBuffer + strlen(g_errorTextBuffer);
	while (p > g_errorTextBuffer&&isspace(*p))
		*--p = 0;

	return g_errorTextBuffer;
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

static char *GetLastErrorText(void)
{
	return GetErrorText(GetLastError());
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

static const char *GetMMRESULTText(MMRESULT result)
{
#define CASE(X) case (X): return #X
	switch (result)
	{
	default:
		return "?MMRESULT?";
		CASE(MMSYSERR_NOERROR);
		CASE(MMSYSERR_ERROR);
		CASE(MMSYSERR_BADDEVICEID);
		CASE(MMSYSERR_NOTENABLED);
		CASE(MMSYSERR_ALLOCATED);
		CASE(MMSYSERR_INVALHANDLE);
		CASE(MMSYSERR_NODRIVER);
		CASE(MMSYSERR_NOMEM);
		CASE(MMSYSERR_NOTSUPPORTED);
		CASE(MMSYSERR_BADERRNUM);
		CASE(MMSYSERR_INVALFLAG);
		CASE(MMSYSERR_INVALPARAM);
		CASE(MMSYSERR_HANDLEBUSY);
		CASE(MMSYSERR_INVALIDALIAS);
		CASE(MMSYSERR_BADDB);
		CASE(MMSYSERR_KEYNOTFOUND);
		CASE(MMSYSERR_READERROR);
		CASE(MMSYSERR_WRITEERROR);
		CASE(MMSYSERR_DELETEERROR);
		CASE(MMSYSERR_VALNOTFOUND);
		CASE(MMSYSERR_NODRIVERCB);
		CASE(MMSYSERR_MOREDATA);
	}
#undef CASE
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

typedef BOOL(*WavChunkFn)(const char *id, const uint8_t *data, uint32_t size, void *context);

static BOOL ForEachWavFileChunk(
	const void *wav_data_arg,
	size_t wav_data_size,
	WavChunkFn chunk_fn,
	void *context,
	char *err_text,
	size_t err_text_size)
{
	const char *wav_data = (const char *)wav_data_arg;

	BOOL good = FALSE;

	if (wav_data_size >= 12 &&
		strncmp(wav_data, "RIFF", 4) == 0 &&
		strncmp(wav_data + 8, "WAVE", 4) == 0)
	{
		uint32_t riff_size = *(const uint32_t *)(wav_data + 4);
		if (riff_size <= wav_data_size - 8)
		{
			uint32_t chunk_offset = 4;

			good = TRUE;

			while (chunk_offset < riff_size)
			{
				const char *header = wav_data + 8 + chunk_offset;

				char id[5];
				memcpy(id, header, 4);
				id[4] = 0;

				uint32_t size = *(const uint32_t *)(header + 4);

				if (!(*chunk_fn)(id, header + 8, size, context))
				{
					good = FALSE;
					break;
				}

				if (size % 2 != 0)
				{
					// odd-sized chunks are followed by a pad byte
					++size;
				}

				chunk_offset += 8 + size;
			}

			ASSERT(chunk_offset == riff_size);
		}
	}
	else
	{
		snprintf(err_text, err_text_size, "not a WAV file");
	}

	return good;
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

static BOOL GetFmtAndData(const char *id, const uint8_t *data, uint32_t size, void *context)
{
	WavFile *wav = context;

	if (strcmp(id, "fmt ") == 0)
	{
		wav->fmt = (const WAVEFORMATEX *)data;
	}
	else if (strcmp(id, "data") == 0)
	{
		wav->data = data;
		wav->dataSize = size;
	}

	return TRUE;
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

static void CALLBACK WaveOutProc(
	HWAVEOUT hWaveOut,
	UINT uMsg,
	DWORD_PTR dwInstance,
	DWORD_PTR dwParam1,
	DWORD_PTR dwParam2)
{

}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

static LRESULT CALLBACK HandleKeyboard(int nCode, WPARAM wParam, LPARAM lParam)
{
	KBDLLHOOKSTRUCT *ev = (KBDLLHOOKSTRUCT *)lParam;

	BOOL down = wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN;

	if (ev->scanCode < NUM_KEYS)
	{
		BOOL *flag = &g_keyStates[ev->scanCode];

		if (*flag != down)
		{
			*flag = down;

			WavFile *wav = &g_wavFiles[ev->scanCode][*flag];

			if (wav->buffer)
			{
#if SINGLE_DEVICE
				HWAVEOUT hWaveOut = g_hWaveOut;
#else
				HWAVEOUT hWaveOut = g_hWaveOuts[ev->scanCode];

				waveOutReset(hWaveOut);
#endif

				waveOutWrite(hWaveOut, &wav->wh, sizeof wav->wh);
			}
		}
	}

	return CallNextHookEx(g_hKeyboardHook, nCode, wParam, lParam);
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
	//g_playQueueEvent = CreateEvent(NULL, TRUE, FALSE, NULL);

	char exePath[MAX_PATH];
	{
		GetModuleFileName(GetModuleHandle(NULL), exePath, sizeof exePath);
		PathRemoveFileSpec(exePath);
	}

	{
		for (int i = 0; i < 256; ++i)
		{
			for (int j = 0; j < 2; ++j)
			{
				BOOL good = FALSE;
				WavFile *wav = &g_wavFiles[i][j];

				char fileName[MAX_PATH];
				snprintf(fileName, sizeof fileName, "%s\\wav\\%02x-%d.wav", exePath, i, j);

				HANDLE hFile = CreateFile(fileName, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
					FILE_FLAG_SEQUENTIAL_SCAN, NULL);
				if (hFile == INVALID_HANDLE_VALUE)
					goto next;

				LARGE_INTEGER size;
				if (!GetFileSizeEx(hFile, &size))
					goto next;

				if (size.QuadPart > SIZE_MAX || size.QuadPart > MAXDWORD)
					goto next;

				wav->buffer = malloc((size_t)size.QuadPart);
				if (!wav->buffer)
					goto next;

				DWORD numRead;
				if (!ReadFile(hFile, wav->buffer, (DWORD)size.QuadPart, &numRead, NULL))
					goto next;

				if (numRead != size.QuadPart)
					goto next;

				char err[100];
				if (!ForEachWavFileChunk(wav->buffer, (size_t)size.QuadPart, &GetFmtAndData, wav, err, sizeof err))
					goto next;

				good = TRUE;

			next:;
				CloseHandle(hFile);
				hFile = INVALID_HANDLE_VALUE;

				if (good)
				{
					dprintf("%s: %u bytes, %dHz, %d bits\n",
						fileName, wav->dataSize, wav->fmt->nSamplesPerSec, wav->fmt->wBitsPerSample);
				}
				else
				{
					free(wav->buffer);
					wav->buffer = NULL;

					//dprintf("%s: FAIL\n", fileName);
				}
			}
		}

		size_t total = 0;
		for (int i = 0; i < 256; ++i)
		{
			for (int j = 0; j < 2; ++j)
				total += g_wavFiles[i][j].dataSize;
		}

		dprintf("total: %zu\n", total);
	}

	{
		MMRESULT mr;
		WAVEFORMATEX fmt = {
			.wFormatTag = WAVE_FORMAT_PCM,
			.nChannels = 2,
			.wBitsPerSample = 16,
			.nSamplesPerSec = 44100,
			.nBlockAlign = 4,
		};
		fmt.nAvgBytesPerSec = fmt.nSamplesPerSec*fmt.nBlockAlign;

#if SINGLE_DEVICE
		mr = waveOutOpen(&g_hWaveOut, WAVE_MAPPER, &fmt, (DWORD_PTR)&WaveOutProc, 0, CALLBACK_FUNCTION);
		if (mr != MMSYSERR_NOERROR)
		{
			Fatal("waveOutOpen failed: %s\n", GetMMRESULTText(mr));
			return 1;
		}
#else
		for (int i = 0; i < NUM_KEYS; ++i)
		{
			WavFile *wavs = g_wavFiles[i];

			if (!wavs[0].buffer && !wavs[1].buffer)
				continue;

			mr = waveOutOpen(&g_hWaveOuts[i], WAVE_MAPPER, &fmt,
				(DWORD_PTR)&WaveOutProc, 0, CALLBACK_FUNCTION);
			if (mr != MMSYSERR_NOERROR)
			{
				Fatal("waveOutOpen (%d) failed: %s\n", i, GetMMRESULTText(mr));
				return 1;
			}
		}
#endif

		for (int i = 0; i < 256; ++i)
		{
			for (int j = 0; j < 2; ++j)
			{
				WavFile *w = &g_wavFiles[i][j];
				HWAVEOUT hWaveOut;

				if (!w->buffer)
					continue;

				w->wh.lpData = (LPSTR)w->data;
				w->wh.dwBufferLength = w->dataSize;
				w->wh.dwBytesRecorded = w->wh.dwBufferLength;
				w->wh.dwUser = 0;

#if SINGLE_DEVICE
				hWaveOut = g_hWaveOut;
#else
				hWaveOut = g_hWaveOuts[i];
#endif

				mr = waveOutPrepareHeader(hWaveOut, &w->wh, sizeof w->wh);
				if (mr != MMSYSERR_NOERROR)
				{
					Fatal("waveOutPrepareHeader failed for %02X-%d: %s\n",
						i, j, GetMMRESULTText(mr));
					return 1;
				}
			}
		}
	}

	g_hKeyboardHook = SetWindowsHookEx(WH_KEYBOARD_LL, &HandleKeyboard, GetModuleHandle(NULL), 0);

	{
		MSG msg;
		while (GetMessage(&msg, NULL, 0, 0) > 0)
		{
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}
	}

	//waveOutWrite(hWaveOut, &g_wavFiles[10][0].wh, sizeof g_wavFiles[10][0].wh);

	//Sleep(500);

	return 0;
}
