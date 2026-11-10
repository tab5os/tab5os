# Media and USB

## Audio output

Include `<t5/audio.h>`. Needs no permission.

```c
t5_audio_out_cfg_t cfg = { .rate = 16000, .channels = 1 };   /* 8000..48000, 1 or 2 channels, 16-bit LE */
t5_audio_out_t *out;
if (t5_audio_out_open(&cfg, &out) == T5_OK) {
    int n = t5_audio_out_write(out, pcm, bytes, 100);        /* bytes accepted, or a negative error */
    t5_audio_out_drain(out, 1000);
    T5_IGNORE(t5_audio_out_close(out));
}
```

| Fact | Detail |
| --- | --- |
| Streams | Up to 4 (`T5_AUDIO_OUT_MAX`) open at once, every app's together, mixed. `T5_ERR_BUSY` past that |
| Usage | `t5_audio_out_open` is MEDIA. `t5_audio_out_open_usage(&cfg, T5_AUDIO_USAGE_ALERT or SPEECH, &out)` ducks MEDIA streams by 12 dB while it has sound; system sounds do too |
| Level | `t5_audio_out_set_volume(out, pct)`: the stream's own level under the system volume |
| Conversion | Any supported format is converted to 48 kHz stereo for you |
| Buffer | `buffer_ms` defaults to 200 |
| Wake lock | An open stream holds an audio lock, so it plays with the screen off. `t5_audio_out_pause` releases it |
| Routing | Speaker, or headphones when plugged in, automatic. `T5_EV_AUDIO_ROUTE` says when it changes, `T5_EV_AUDIO_VOLUME` when the volume or mute does |
| Mixing | Streams and system sounds are summed and clipped to 16 bits: leave headroom if several play at once |
| Bluetooth | No Bluetooth audio: the C6 radio is Bluetooth LE only (no A2DP), and LE Audio isn't available on it |

Write from a task, since `t5_audio_out_write` blocks until there's room. That also paces your loop. If it runs dry you hear a gap: `audio` on the console reports `underruns`. A longer `buffer_ms` buys tolerance at the cost of latency. See the [synth recipe](Cookbook.md#audio-synth).

| Helper | Use |
| --- | --- |
| `t5_audio_play_sound(s)` | A system click or notification, honouring the person's setting |
| `t5_audio_tone(hz, ms)` | A test tone |
| `t5_audio_volume`, `t5_audio_set_volume`, `t5_audio_muted`, `t5_audio_headphones` | Output state |
| `t5_resample`, `t5_pcm_downmix`, `t5_mix_add`, `t5_pcm_peak_dbfs` | DSP building blocks |

## Sound files

`t5_audio_play_file(path, usage, &player)` (or `t5_audio_play_mem` for an asset in memory) decodes and plays on a task of its own; `t5_audio_player_pause`, `_seek`, `_set_volume`, `_state` (playing, paused, ended, error, and the position), `_wait` and `_close` control it. `t5_audio_dec_open` / `_read` / `_seek` give the PCM to your code instead.

| Format | Support |
| --- | --- |
| WAV | PCM 8/16/24/32-bit, float 32/64-bit, `WAVE_FORMAT_EXTENSIBLE`; any rate (the player needs 8-48 kHz); up to 8 channels, the first two used |
| MP3 | Not in this OS build (`t5_audio_format_supported(T5_AUDIO_FMT_MP3)` is false): the decoder doesn't fit the image budget. Compile one into your app (minimp3 is a single header) and play its PCM through a stream |

Files follow your app's file rules, and decoders and players close when the app exits (4 per app). `audio wav /sd/t.wav 2 440` and `audio file /sd/t.wav` on the console make and play a test file; `audio duck` measures the ducking.

## Media sessions

A player app opens a media session (`t5_media_session_open`), publishes what is playing with `_set_info` and `_set_state`, and reads commands with `_next` on a task of its own. The standby screen's music card shows the active session and sends it play, pause, next and previous. The session doesn't play anything itself; your app does, and it reports back with `_set_state`. The details are in the [SDK guide](../sdk.md#media-sessions), and [Music](https://github.com/tab5os/music) is a complete player. On the console, `media` shows the active session, and `media play`, `pause`, `toggle`, `next`, `prev` and `stop` send it commands.

## Microphone

Declare `mic = true`, and [ask at run time](Manifest-and-Packaging.md#permissions-the-person-is-asked-for).

```c
t5_audio_in_cfg_t cfg = { .rate = 16000, .channels = 1, .aec = true };
t5_audio_in_t *in;
if (t5_audio_in_open(&cfg, &in) == T5_OK) {
    int n = t5_audio_in_read(in, pcm, sizeof pcm, 100);
    T5_IGNORE(t5_audio_in_close(in));
}
```

Echo cancellation, `aec`, works at 16 kHz or lower, mono. The status bar shows an indicator while recording. A reader that's too slow loses audio, so read from a dedicated task and hand buffers to a slower writer.

## Camera

Include `<t5/camera.h>`. Declare `camera = true`, and ask at run time. One owner at a time.

```c
t5_camera_t *cam;
if (t5_camera_open(&cam) != T5_OK) return;
lv_obj_t *img = t5_camera_preview_create(cam, parent);    /* UI task */
```

| Operation | Rule |
| --- | --- |
| Preview | Scaled to fit and kept upright. Deleting the object stops it. 11 to 13 fps through LVGL |
| `t5_camera_photo(cam, path, quality)` | Blocks until written. **Any task but the UI task** |
| `t5_camera_record_start` / `_stop` | H.264 Annex B to a file, 720p at 30 fps. Both block and need the UI task to be free. **Any task but the UI task** |
| Suspend | **Close in `on_suspend`, and reopen in `on_resume`.** The OS closes it at Sleep and exit if you don't |
| File paths | Photos and recordings follow the [file rules](Storage.md#which-files-an-app-can-use): `/sd/DCIM` needs `shared_write` |

Output is an elementary H.264 stream, not an MP4. The bitrate defaults to 4000 kbps and lowers if the card can't keep up. Check `t5_camera_stats` for drops.

Full recipe: [Camera with permission](Cookbook.md#camera-with-permission).

## JPEG

`t5_jpeg_size` and `t5_jpeg_decode` use the hardware decoder and produce RGB565. Decoded dimensions are rounded up to 16 px. Call them from a worker task. See [images](UI-Guide.md#images).

## USB-A host

Include `<t5/usb.h>`. Serial, MIDI and UVC need `usb = true`.

| Device | Use |
| --- | --- |
| Keyboard, mouse | Automatic: they become LVGL input devices |
| Drive | `/usb`; check `t5_usb_storage_mounted` |
| CDC serial | `t5_usb_serial_open(baud, &s)`, then read and write with timeouts |
| MIDI | `t5_usb_midi_listen(cb, arg)`, `t5_usb_midi_send(msg, len)` |
| UVC camera | `t5_usb_uvc_open(w, h, fps, cb, arg, &u)`: MJPEG frames |

```c
t5_usb_dev_t devs[T5_USB_MAX_DEVICES];
int n = t5_usb_devices(devs, T5_USB_MAX_DEVICES);
```

**UVC and MIDI callbacks run on the USB driver's task.** Return quickly and never touch LVGL there. Copy the frame and signal a worker. The UVC Monitor example is a clean model: drop frames while a decode is in flight, decode to the hidden buffer, and swap on the UI task.

USB power is dropped in Sleep, so devices re-enumerate after wake. Reopen on `T5_EV_USB_ATTACHED`, or by polling `t5_usb_serial_present` and so on from a timer.
