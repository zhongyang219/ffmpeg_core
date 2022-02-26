#include "wasapi.h"

#if __MINGW32__
#include <initguid.h>
#endif
#include <Functiondiscoverykeys_devpkey.h>
#include <malloc.h>
#include <string.h>
#include <wchar.h>
#include "cpp2c.h"
#include "err.h"
#include "linked_list.h"
#include "output.h"
#include "wchar_util.h"

#define GET_WIN_ERROR(errmsg, hr) std::string errmsg; \
if (!err::get_winerror(errmsg, hr)) errmsg = "(null)"; \
else { \
    while (errmsg.back() == '\n' || errmsg.back() == '\r') { \
        errmsg.erase(errmsg.end() - 1); \
    } \
}

#ifndef min
#define min(a,b) (((a) < (b)) ? (a) : (b))
#endif

template <typename T>
void comfree(T*& data) {
    if (data) data->Release();
    data = nullptr;
}

template <typename T>
void zeromem(T* data) {
    memset(data, 0, sizeof(T));
}

template <typename T>
void memcpy(T* dest, T* src) {
    memcpy(dest, src, sizeof(T));
}

const CLSID CLSID_MMDeviceEnumerator = __uuidof(MMDeviceEnumerator);
const IID IID_IMMDeviceEnumerator = __uuidof(IMMDeviceEnumerator);
const IID IID_IAudioClient = __uuidof(IAudioClient);
const IID IID_IAudioRenderClient = __uuidof(IAudioRenderClient);

/// 是否已经初始化
static bool wasapi_initialzed = false;
/// 被初始化的次数
static int wasapi_init_count = 0;
/// 默认设备
static IMMDevice* default_device = nullptr;
/// 设备列表
static struct LinkedList<WASAPIDevice*>* devices = nullptr;

void free_WASAPIHandle(WASAPIHandle* handle) {
    if (!handle) return;
    if (handle->thread) {
        DWORD status;
        while (GetExitCodeThread(handle->thread, &status)) {
            if (status == STILL_ACTIVE) {
                status = 0;
                handle->stoping = 1;
                Sleep(10);
            } else {
                break;
            }
        }
    }
    comfree(handle->render);
    comfree(handle->client);
    free(handle);
}

int init_WASAPI() {
    if (wasapi_initialzed) {
        wasapi_init_count += 1;
        return FFMPEG_CORE_ERR_OK;
    }
    int re = FFMPEG_CORE_ERR_OK;
    HRESULT hr;
    IMMDeviceEnumerator* enu;
    IMMDeviceCollection* devicecol;
    if (!SUCCEEDED(hr = CoInitializeEx(NULL, COINIT_MULTITHREADED))) {
        GET_WIN_ERROR(errmsg, hr);
        av_log(NULL, AV_LOG_FATAL, "Failed to initilize COM enviornment: %s (%lu).\n", errmsg.c_str(), hr);
        re = FFMPEG_CORE_ERR_FAILED_INIT_WASAPI;
        goto end;
    }
    if (!SUCCEEDED(hr = CoCreateInstance(CLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL, IID_IMMDeviceEnumerator, (void**)&enu))) {
        GET_WIN_ERROR(errmsg, hr);
        av_log(NULL, AV_LOG_FATAL, "Failed to create device enumerator: %s (%lu).\n", errmsg.c_str(), hr);
        re = FFMPEG_CORE_ERR_FAILED_INIT_WASAPI;
        goto end;
    }
    if (!SUCCEEDED(hr = enu->GetDefaultAudioEndpoint(eRender, eMultimedia, &default_device))) {
        GET_WIN_ERROR(errmsg, hr);
        av_log(NULL, AV_LOG_FATAL, "Failed to get default audio device: %s (%lu).\n", errmsg.c_str(), hr);
        re = FFMPEG_CORE_ERR_FAILED_INIT_WASAPI;
        goto end;
    }
    if (!SUCCEEDED(hr = enu->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &devicecol))) {
        GET_WIN_ERROR(errmsg, hr);
        av_log(NULL, AV_LOG_FATAL, "Failed to get audio devices: %s (%lu).\n", errmsg.c_str(), hr);
        re = FFMPEG_CORE_ERR_FAILED_INIT_WASAPI;
        goto end;
    }
    if (devicecol) {
        unsigned int count = 0;
        if (!SUCCEEDED(hr = devicecol->GetCount(&count))) {
            GET_WIN_ERROR(errmsg, hr);
            av_log(NULL, AV_LOG_FATAL, "Failed to get audio devices' count: %s (%lu).\n", errmsg.c_str(), hr);
            re = FFMPEG_CORE_ERR_FAILED_INIT_WASAPI;
            goto end;
        }
        for (unsigned int i = 0; i < count; i++) {
            IMMDevice* tmp = nullptr;
            WASAPIDevice* d = nullptr;
            if (!SUCCEEDED(hr = devicecol->Item(i, &tmp))) {
                GET_WIN_ERROR(errmsg, hr);
                av_log(NULL, AV_LOG_FATAL, "Failed to get audio device index %u: %s (%lu).\n", i, errmsg.c_str(), hr);
                re = FFMPEG_CORE_ERR_FAILED_INIT_WASAPI;
                goto end;
            }
            if ((re = create_WASAPIDevice(tmp, &d))) {
                goto end;
            }
            if (!linked_list_append(devices, &d)) {
                free_WASAPIDevice(d);
                re = FFMPEG_CORE_ERR_OOM;
                goto end;
            }
        }
    }
    wasapi_initialzed = true;
    wasapi_init_count += 1;
end:
    comfree(enu);
    comfree(devicecol);
    if (re != 0) {
        comfree(default_device);
        linked_list_clear(devices, free_WASAPIDevice);
    }
    return re;
}

void uninit_WASAPI() {
    if (!wasapi_init_count || !wasapi_initialzed) return;
    wasapi_init_count -= 1;
    if (!wasapi_init_count) {
        wasapi_initialzed = false;
        comfree(default_device);
        linked_list_clear(devices, free_WASAPIDevice);
    }
}

int create_WASAPIDevice(IMMDevice* device, WASAPIDevice** output) {
    if (!device || !output) {
        comfree(device);
        return FFMPEG_CORE_ERR_NULLPTR;
    }
    WASAPIDevice* d = (WASAPIDevice*)malloc(sizeof(WASAPIDevice));
    IPropertyStore* props = nullptr;
    int re = FFMPEG_CORE_ERR_OK;
    HRESULT hr;
    PROPVARIANT prop;
    bool prop_inited = false;
    if (!d) {
        comfree(device);
        av_log(NULL, AV_LOG_FATAL, "Failed to allcoate WASAPIDevice.\n");
        return FFMPEG_CORE_ERR_OOM;
    }
    zeromem(d);
    d->device = device;
    if (!SUCCEEDED(hr = device->OpenPropertyStore(STGM_READ, &props))) {
        GET_WIN_ERROR(errmsg, hr);
        av_log(NULL, AV_LOG_FATAL, "Failed to get audio device's property: %s (%lu).\n", errmsg.c_str(), hr);
        re = FFMEPG_CORE_ERR_FAILED_GET_WASAPI_DEVICE_PROP;
        goto end;
    }
    PropVariantInit(&prop);
    prop_inited = true;
    if (!SUCCEEDED(hr = props->GetValue(PKEY_Device_FriendlyName, &prop))) {
        GET_WIN_ERROR(errmsg, hr);
        av_log(NULL, AV_LOG_FATAL, "Failed to get audio device's friendly name: %s (%lu).\n", errmsg.c_str(), hr);
        re = FFMEPG_CORE_ERR_FAILED_GET_WASAPI_DEVICE_PROP;
        goto end;
    }
    if (!cpp2c::string2char(prop.pwszVal, d->name)) {
        re = FFMPEG_CORE_ERR_OOM;
        goto end;
    }
    *output = d;
    goto end2;
end:
    free_WASAPIDevice(d);
end2:
    comfree(props);
    if (prop_inited) {
        PropVariantClear(&prop);
    }
    return re;
}

void free_WASAPIDevice(WASAPIDevice* device) {
    if (!device) return;
    comfree(device->device);
    if (device->name) {
        free(device->name);
    }
    free(device);
}

int copy_IMMDevice(IMMDevice* src, IMMDevice** out) {
    if (!src || !out) return FFMPEG_CORE_ERR_NULLPTR;
    LPWSTR id = nullptr;
    IMMDeviceEnumerator* enu = nullptr;
    int re = FFMPEG_CORE_ERR_OK;
    HRESULT hr;
    if (!SUCCEEDED(hr = src->GetId(&id))) {
        GET_WIN_ERROR(errmsg, hr);
        av_log(NULL, AV_LOG_FATAL, "Failed to get audio device's id: %s (%lu).\n", errmsg.c_str(), hr);
        re = FFMEPG_CORE_ERR_FAILED_GET_WASAPI_DEVICE_PROP;
        goto end;
    }
    if (!SUCCEEDED(hr = CoCreateInstance(CLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL, IID_IMMDeviceEnumerator, (void**)&enu))) {
        GET_WIN_ERROR(errmsg, hr);
        av_log(NULL, AV_LOG_FATAL, "Failed to create device enumerator: %s (%lu).\n", errmsg.c_str(), hr);
        re = FFMPEG_CORE_ERR_FAILED_INIT_WASAPI;
        goto end;
    }
    if (!SUCCEEDED(hr = enu->GetDevice(id, out))) {
        GET_WIN_ERROR(errmsg, hr);
        av_log(NULL, AV_LOG_FATAL, "Failed to get device from id: %s (%lu).\n", errmsg.c_str(), hr);
        re = FFMPEG_CORE_ERR_FAILED_INIT_WASAPI;
        goto end;
    }
end:
    if (id) CoTaskMemFree(id);
    comfree(enu);
    return re;
}

bool comp_device_name(WASAPIDevice* device, const wchar_t* name) {
    if (!device || !device->name || !name) return false;
    return !wcscmp(device->name, name);
}

int get_Device(const wchar_t* name, IMMDevice** result) {
    if (!result) return FFMPEG_CORE_ERR_NULLPTR;
    if (!wasapi_initialzed) return FFMPEG_CORE_ERR_WASAPI_NOT_INITED;
    if (!name) {
        *result = default_device;
        return FFMPEG_CORE_ERR_OK;
    }
    auto d = linked_list_get(devices, name, &comp_device_name);
    if (!d) return FFMPEG_CORE_ERR_WASAPI_DEVICE_NOT_FOUND;
    *result = d->d->device;
    return FFMPEG_CORE_ERR_OK;
}

int open_WASAPI_device(MusicHandle* handle, const wchar_t* name) {
    if (!handle) return FFMPEG_CORE_ERR_NULLPTR;
    handle->wasapi = (WASAPIHandle*)malloc(sizeof(WASAPIHandle));
    if (!handle->wasapi) return FFMPEG_CORE_ERR_OOM;
    zeromem(handle->wasapi);
    IMMDevice* ori = nullptr, * device = nullptr;
    int re = FFMPEG_CORE_ERR_OK;
    HRESULT hr;
    WAVEFORMATEXTENSIBLE fmt;
    WAVEFORMATEX* target_fmt = nullptr;
    REFERENCE_TIME period = 0;
    zeromem(&fmt);
    if ((re = get_Device(name, &ori))) {
        goto end;
    }
    if (!ori) {
        re = FFMPEG_CORE_ERR_WASAPI_DEVICE_NOT_FOUND;
        goto end;
    }
    if ((re = copy_IMMDevice(ori, &device))) {
        goto end;
    }
    if (!SUCCEEDED(hr = device->Activate(IID_IAudioClient, CLSCTX_ALL, nullptr, (void**)&handle->wasapi->client))) {
        GET_WIN_ERROR(errmsg, hr);
        av_log(NULL, AV_LOG_FATAL, "Failed to active device: %s (%lu).\n", errmsg.c_str(), hr);
        re = FFMPEG_CORE_ERR_WASAPI_FAILED_OPEN_DEVICE;
        goto end;
    }
    if ((re = get_format_info(handle->decoder, &fmt))) {
        goto end;
    }
    for (int i = 0; i < 16; i++) {
        if ((re = check_format_supported(handle->wasapi->client, handle->s->enable_exclusive, &fmt, &target_fmt, i))) {
            goto end;
        }
        if (target_fmt || handle->s->enable_exclusive) {
            break;
        }
    }
    if (!target_fmt) {
        re = FFMPEG_CORE_ERR_WASAPI_NO_SUITABLE_FORMAT;
        goto end;
    }
    if (handle->s->enable_exclusive && !SUCCEEDED(handle->wasapi->client->GetDevicePeriod(&period, nullptr))) {
        GET_WIN_ERROR(errmsg, hr);
        av_log(NULL, AV_LOG_FATAL, "Failed to get device's period: %s (%lu).\n", errmsg.c_str(), hr);
        re = FFMPEG_CORE_ERR_WASAPI_FAILED_OPEN_DEVICE;
        goto end;
    }
    if (!SUCCEEDED(hr = handle->wasapi->client->Initialize(handle->s->enable_exclusive ? AUDCLNT_SHAREMODE_EXCLUSIVE : AUDCLNT_SHAREMODE_SHARED, AUDCLNT_SESSIONFLAGS_EXPIREWHENUNOWNED | AUDCLNT_SESSIONFLAGS_DISPLAY_HIDEWHENEXPIRED | AUDCLNT_STREAMFLAGS_EVENTCALLBACK, handle->s->enable_exclusive ? period : 0, period, target_fmt, nullptr)) && hr != AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
        GET_WIN_ERROR(errmsg, hr);
        av_log(NULL, AV_LOG_FATAL, "Failed to initialize the audio client: %s (%lu).\n", errmsg.c_str(), hr);
        re = FFMPEG_CORE_ERR_WASAPI_FAILED_OPEN_DEVICE;
        goto end;
    }
    if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
        uint32_t tmp;
        REFERENCE_TIME tmp2;
        if (!SUCCEEDED(hr = handle->wasapi->client->GetBufferSize(&tmp))) {
            GET_WIN_ERROR(errmsg, hr);
            av_log(NULL, AV_LOG_FATAL, "Failed to get the buffer size of audio client: %s (%lu).\n", errmsg.c_str(), hr);
            re = FFMPEG_CORE_ERR_WASAPI_FAILED_OPEN_DEVICE;
            goto end;
        }
        comfree(handle->wasapi->client);
        tmp2 = (REFERENCE_TIME)((10000.0 * 1000 / target_fmt->nSamplesPerSec * tmp) + 0.5);
        if (!SUCCEEDED(hr = device->Activate(IID_IAudioClient, CLSCTX_ALL, nullptr, (void**)&handle->wasapi->client))) {
            GET_WIN_ERROR(errmsg, hr);
            av_log(NULL, AV_LOG_FATAL, "Failed to active device: %s (%lu).\n", errmsg.c_str(), hr);
            re = FFMPEG_CORE_ERR_WASAPI_FAILED_OPEN_DEVICE;
            goto end;
        }
        if (!SUCCEEDED(hr = handle->wasapi->client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_SESSIONFLAGS_EXPIREWHENUNOWNED | AUDCLNT_SESSIONFLAGS_DISPLAY_HIDEWHENEXPIRED | AUDCLNT_STREAMFLAGS_EVENTCALLBACK, tmp2, tmp2, target_fmt, nullptr))) {
            GET_WIN_ERROR(errmsg, hr);
            av_log(NULL, AV_LOG_FATAL, "Failed to initialize the audio client: %s (%lu).\n", errmsg.c_str(), hr);
            re = FFMPEG_CORE_ERR_WASAPI_FAILED_OPEN_DEVICE;
            goto end;
        }
    }
    if (!SUCCEEDED(hr = handle->wasapi->client->GetBufferSize(&handle->wasapi->frame_count))) {
        GET_WIN_ERROR(errmsg, hr);
        av_log(NULL, AV_LOG_FATAL, "Failed to get audio client's buffer size: %s (%lu).\n", errmsg.c_str(), hr);
        re = FFMPEG_CORE_ERR_WASAPI_FAILED_OPEN_DEVICE;
        goto end;
    }
    if (!SUCCEEDED(hr = handle->wasapi->client->GetService(IID_IAudioRenderClient, (void**)&handle->wasapi->render))) {
        GET_WIN_ERROR(errmsg, hr);
        av_log(NULL, AV_LOG_FATAL, "Failed to get renderer from audio client: %s (%lu).\n", errmsg.c_str(), hr);
        re = FFMPEG_CORE_ERR_WASAPI_FAILED_OPEN_DEVICE;
        goto end;
    }
    handle->sdl_spec.freq = target_fmt->nSamplesPerSec;
    handle->sdl_spec.channels = target_fmt->nChannels;
    if (target_fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        WAVEFORMATEXTENSIBLE* tmp = (WAVEFORMATEXTENSIBLE*)target_fmt;
        if (tmp->dwChannelMask && av_get_channel_layout_nb_channels(tmp->dwChannelMask) == target_fmt->nChannels) {
            handle->output_channel_layout = tmp->dwChannelMask;
        } else {
            handle->output_channel_layout = av_get_default_channel_layout(target_fmt->nChannels);
        }
        if (tmp->SubFormat == KSDATAFORMAT_SUBTYPE_PCM) {
#define RETURN_DATA(s) handle->target_format = s; break;
            switch (target_fmt->wBitsPerSample) {
                case 8:
                    RETURN_DATA(AV_SAMPLE_FMT_U8);
                case 16:
                    RETURN_DATA(AV_SAMPLE_FMT_S16);
                case 32:
                    RETURN_DATA(AV_SAMPLE_FMT_S32);
                case 64:
                    RETURN_DATA(AV_SAMPLE_FMT_S64);
                default:
                    re = FFMPEG_CORE_ERR_WASAPI_NO_SUITABLE_FORMAT;
                    goto end;
            }

        } else if (tmp->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) {
            switch (target_fmt->wBitsPerSample) {
                case 32:
                    RETURN_DATA(AV_SAMPLE_FMT_FLT);
                case 64:
                    RETURN_DATA(AV_SAMPLE_FMT_DBL);
                default:
                    re = FFMPEG_CORE_ERR_WASAPI_NO_SUITABLE_FORMAT;
                    goto end;
            }
#undef RETURN_DATA
        } else {
            re = FFMPEG_CORE_ERR_WASAPI_NO_SUITABLE_FORMAT;
            goto end;
        }
    } else {
        handle->output_channel_layout = av_get_default_channel_layout(target_fmt->nChannels);
        if (target_fmt->wFormatTag == WAVE_FORMAT_PCM) {
#define RETURN_DATA(s) handle->target_format = s; break;
            switch (target_fmt->wBitsPerSample) {
            case 8:
                RETURN_DATA(AV_SAMPLE_FMT_U8);
            case 16:
                RETURN_DATA(AV_SAMPLE_FMT_S16);
            case 32:
                RETURN_DATA(AV_SAMPLE_FMT_S32);
            case 64:
                RETURN_DATA(AV_SAMPLE_FMT_S64);
            default:
                re = FFMPEG_CORE_ERR_WASAPI_NO_SUITABLE_FORMAT;
                goto end;
            }

        } else if (target_fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
            switch (target_fmt->wBitsPerSample) {
            case 32:
                RETURN_DATA(AV_SAMPLE_FMT_FLT);
            case 64:
                RETURN_DATA(AV_SAMPLE_FMT_DBL);
            default:
                re = FFMPEG_CORE_ERR_WASAPI_NO_SUITABLE_FORMAT;
                goto end;
            }
#undef RETURN_DATA
        } else {
            re = FFMPEG_CORE_ERR_WASAPI_NO_SUITABLE_FORMAT;
            goto end;
        }
    }
    handle->target_format_pbytes = av_get_bytes_per_sample(handle->target_format);
    handle->wasapi->eve = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (!handle->wasapi->eve) {
        re = FFMPEG_CORE_ERR_FAILED_CREATE_EVENT;
        goto end;
    }
    if (!SUCCEEDED(hr = handle->wasapi->client->SetEventHandle(handle->wasapi->eve))) {
        GET_WIN_ERROR(errmsg, hr);
        av_log(NULL, AV_LOG_FATAL, "Failed to set event handle to audio client: %s (%lu).\n", errmsg.c_str(), hr);
        re = FFMPEG_CORE_ERR_WASAPI_FAILED_OPEN_DEVICE;
        goto end;
    }
    handle->wasapi->thread = CreateThread(nullptr, 0, wasapi_loop, handle, 0, nullptr);
    if (!handle->wasapi->thread) {
        re = FFMPEG_CORE_ERR_FAILED_CREATE_THREAD;
        goto end;
    }
end:
    comfree(device);
    if (target_fmt) {
        CoTaskMemFree(target_fmt);
    }
    if (re) {
        free_WASAPIHandle(handle->wasapi);
        handle->wasapi = nullptr;
    }
    return re;
}

void close_WASAPI_device(MusicHandle* handle) {
    if (!handle || !handle->wasapi) return;
    free_WASAPIHandle(handle->wasapi);
    handle->wasapi = nullptr;
}

int32_t get_WASAPI_channel_mask(int64_t channel_layout, int channels) {
    if (channel_layout == 0) channel_layout = av_get_default_channel_layout(channels);
    return channel_layout & (int32_t)0xffffffff;
}

int format_is_pcm(enum AVSampleFormat fmt) {
    switch (fmt) {
        case AV_SAMPLE_FMT_U8:
        case AV_SAMPLE_FMT_S16:
        case AV_SAMPLE_FMT_S32:
        case AV_SAMPLE_FMT_U8P:
        case AV_SAMPLE_FMT_S16P:
        case AV_SAMPLE_FMT_S32P:
        case AV_SAMPLE_FMT_S64:
        case AV_SAMPLE_FMT_S64P:
            return 1;
        default:
            return 0;
    }
}

int get_format_info(AVCodecContext* context, WAVEFORMATEXTENSIBLE* format) {
    if (!context || !format) return FFMPEG_CORE_ERR_NULLPTR;
    format->Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    AVSampleFormat fmt = convert_to_sdl_supported_format(context->sample_fmt);
    format->Format.nChannels = context->channels;
    format->Format.nSamplesPerSec = context->sample_rate;
    format->Format.wBitsPerSample = av_get_bytes_per_sample(fmt) * 8;
    format->Format.nBlockAlign = format->Format.wBitsPerSample * context->channels / 8;
    format->Format.nAvgBytesPerSec = format->Format.nBlockAlign * context->sample_rate;
    format->Format.cbSize = 22;
    format->Samples.wValidBitsPerSample = format->Format.wBitsPerSample;
    format->dwChannelMask = get_WASAPI_channel_mask(context->channel_layout, context->channels);
    format->SubFormat = format_is_pcm(fmt) ? KSDATAFORMAT_SUBTYPE_PCM : KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    return FFMPEG_CORE_ERR_OK;
}

int check_format_supported(IAudioClient* client, int exclusive, WAVEFORMATEXTENSIBLE* base, WAVEFORMATEX** result, int change) {
    if (!client || !base || !result) return FFMPEG_CORE_ERR_NULLPTR;
    HRESULT hr;
    WAVEFORMATEXTENSIBLE fmt;
    memcpy(&fmt, base);
    if (change & WASAPI_FORMAT_CHANGE_FORMAT) {
        fmt.SubFormat = fmt.SubFormat == KSDATAFORMAT_SUBTYPE_PCM ? KSDATAFORMAT_SUBTYPE_IEEE_FLOAT : KSDATAFORMAT_SUBTYPE_PCM;
        if (fmt.SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) {
            fmt.Format.wBitsPerSample = 32;
            fmt.Samples.wValidBitsPerSample = 32;
        } else {
            fmt.Format.wBitsPerSample = 16;
            fmt.Samples.wValidBitsPerSample = 16;
        }
    }
    if (change & WASAPI_FORMAT_CHANGE_BITS) {
        if (fmt.SubFormat == KSDATAFORMAT_SUBTYPE_PCM) {
            fmt.Format.wBitsPerSample = 16;
            fmt.Samples.wValidBitsPerSample = 16;
        }
    }
    if (change & WASAPI_FORMAT_CHANGE_CHANNELS) {
        fmt.Format.nChannels = 2;
        fmt.dwChannelMask = get_WASAPI_channel_mask(0, 2);
    }
    if (change & WASAPI_FORMAT_CHANGE_SAMPLE_RATES) {
        fmt.Format.nSamplesPerSec = 48000;
    }
    fmt.Format.nBlockAlign = fmt.Format.wBitsPerSample * fmt.Format.nChannels / 8;
    fmt.Format.nAvgBytesPerSec = fmt.Format.nBlockAlign * fmt.Format.nSamplesPerSec;
    hr = client->IsFormatSupported(exclusive ? AUDCLNT_SHAREMODE_EXCLUSIVE : AUDCLNT_SHAREMODE_SHARED, (const WAVEFORMATEX*)&fmt, exclusive ? nullptr : result);
    if (hr == S_OK) {
        WAVEFORMATEXTENSIBLE* tmp = (WAVEFORMATEXTENSIBLE*)CoTaskMemAlloc(sizeof(WAVEFORMATEXTENSIBLE));
        memcpy(tmp, &fmt);
        *result = (WAVEFORMATEX*)tmp;
        return FFMPEG_CORE_ERR_OK;
    } else if (hr == S_FALSE) {
        return FFMPEG_CORE_ERR_OK;
    } else if (hr == AUDCLNT_E_UNSUPPORTED_FORMAT) {
        return FFMPEG_CORE_ERR_OK;
    } else {
        GET_WIN_ERROR(errmsg, hr);
        av_log(NULL, AV_LOG_FATAL, "Failed to check format is supported by device: %s (%lu).\n", errmsg.c_str(), hr);
        return FFMPEG_CORE_ERR_WASAPI_FAILED_OPEN_DEVICE;
    }
}

int init_wasapi_output(MusicHandle* handle, const char* device) {
    if (!handle) return FFMPEG_CORE_ERR_NULLPTR;
    std::string d;
    if (device) d = device;
    int re = FFMPEG_CORE_ERR_OK;
    std::wstring tmp;
    if (!d.empty() && !wchar_util::str_to_wstr(tmp, d, CP_UTF8)) {
        re = FFMPEG_CORE_ERR_INVALID_DEVICE_NAME;
        goto end;
    }
    if ((re = init_WASAPI())) {
        goto end;
    }
    handle->wasapi_initialized = 1;
    if ((re = open_WASAPI_device(handle, tmp.empty() ? nullptr : tmp.c_str()))) {
        goto end;
    }
    handle->swrac = swr_alloc_set_opts(NULL, handle->output_channel_layout, handle->target_format, handle->sdl_spec.freq, handle->decoder->channel_layout, handle->decoder->sample_fmt, handle->decoder->sample_rate, 0, NULL);
    if (!handle->swrac) {
        av_log(NULL, AV_LOG_FATAL, "Failed to allocate resample context.\n");
        re = FFMPEG_CORE_ERR_OOM;
        goto end;
    }
    if ((re = swr_init(handle->swrac)) < 0) {
        goto end;
    }
    if (!(handle->buffer = av_audio_fifo_alloc(handle->target_format, handle->sdl_spec.channels, 1))) {
        av_log(NULL, AV_LOG_FATAL, "Failed to allocate buffer.\n");
        re = FFMPEG_CORE_ERR_OOM;
        goto end;
    }
end:
    return re;
}

#define DEAL_WASAPI_LOOP_ERROR(expr) if (!SUCCEEDED(expr)) { \
w->have_err = 1; \
w->err = hr; \
goto end; \
}

DWORD WINAPI wasapi_loop(LPVOID handle) {
    MusicHandle* h = (MusicHandle*)handle;
    WASAPIHandle* w = h->wasapi;
    {
        char buf[AV_TS_MAX_STRING_SIZE];
        AVRational base = { 1, h->sdl_spec.freq };
        av_ts_make_time_string(buf, w->frame_count, &base);
        av_log(NULL, AV_LOG_VERBOSE, "WASAPI buffer length: %s\n", buf);
    }
    while (1) {
        if (w->stoping) break;
        DWORD re = WaitForSingleObject(w->eve, 10);
        if (re == WAIT_TIMEOUT) {
            continue;
        } else if (re == WAIT_OBJECT_0) {
            HRESULT hr;
            uint32_t padding;
            uint8_t* data = nullptr;
            uint32_t count;
            if (!h->s->enable_exclusive) {
                DEAL_WASAPI_LOOP_ERROR(hr = w->client->GetCurrentPadding(&padding));
                count = min(w->frame_count - padding, h->sdl_spec.freq / 100);
                DEAL_WASAPI_LOOP_ERROR(hr = w->render->GetBuffer(count, &data));
            } else {
                DEAL_WASAPI_LOOP_ERROR(hr = w->client->GetBufferSize(&count));
                DEAL_WASAPI_LOOP_ERROR(hr = w->render->GetBuffer(count, &data));
            }
            SDL_callback((void*)handle, data, count * h->target_format_pbytes * h->sdl_spec.channels);
end:
            if (data) {
                if (!SUCCEEDED(hr = w->render->ReleaseBuffer(count, 0))) {
                    w->have_err = 1;
                    w->err = hr;
                }
            }
        } else {
            return FFMPEG_CORE_ERR_WAIT_MUTEX_FAILED;
        }
    }
    return FFMPEG_CORE_ERR_OK;
}

void play_WASAPI_device(MusicHandle* handle, int play) {
    if (!handle || !handle->wasapi) return;
    handle->wasapi->is_playing = play ? 1 : 0;
    if (handle->wasapi->is_playing) handle->wasapi->client->Start();
    else handle->wasapi->client->Stop();
}
