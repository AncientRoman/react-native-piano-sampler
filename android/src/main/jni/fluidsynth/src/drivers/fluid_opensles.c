/* FluidSynth - A Software Synthesizer
 *
 * Copyright (C) 2003  Peter Hanappe and others.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public License
 * as published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the Free
 * Software Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA
 */

/* fluid_opensles.c
 *
 * Audio driver for OpenSLES.
 *
 */

#include "fluid_synth.h"
#include "fluid_adriver.h"
#include "fluid_settings.h"

#include "config.h"

#include <sys/time.h>
#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>

#define NUM_CHANNELS 2

/** fluid_opensles_audio_driver_t
 *
 * This structure should not be accessed directly. Use audio port
 * functions instead.
 */
typedef struct {
    fluid_audio_driver_t driver;
    SLObjectItf engine;
    SLObjectItf output_mix_object;
    SLObjectItf audio_player;
    SLPlayItf audio_player_interface;
    SLAndroidSimpleBufferQueueItf player_buffer_queue_interface;

    void* data;
    int buffer_size;

    int is_sample_format_float;
    int use_callback_mode;

    /* used only by callback mode */
    short* short_buffer;
    short* short_callback_buffer_l;
    short* short_callback_buffer_r;
    float* float_buffer;
    float* float_callback_buffer_l;
    float* float_callback_buffer_r;
    fluid_audio_func_t callback;
    /* used only by non-callback mode */
    fluid_thread_t *thread;

    int cont;
    long next_expected_enqueue_time;

    double sample_rate;
} fluid_opensles_audio_driver_t;


fluid_audio_driver_t* new_fluid_opensles_audio_driver(fluid_settings_t* settings,
                                                      fluid_synth_t* synth);
fluid_audio_driver_t* new_fluid_opensles_audio_driver2(fluid_settings_t* settings,
                                                       fluid_audio_func_t func, void* data);
void delete_fluid_opensles_audio_driver(fluid_audio_driver_t* p);
void fluid_opensles_audio_driver_settings(fluid_settings_t* settings);
static void* fluid_opensles_audio_run(void* d);
static void fluid_opensles_callback(SLAndroidSimpleBufferQueueItf caller, void *pContext);
void fluid_opensles_adjust_latency(fluid_opensles_audio_driver_t* dev);


void fluid_opensles_audio_driver_settings(fluid_settings_t* settings)
{
    fluid_settings_register_int(settings, "audio.opensles.use-callback-mode", 0, 0, 1,
                                FLUID_HINT_TOGGLED);
}


/*
 * new_fluid_opensles_audio_driver
 */
fluid_audio_driver_t*
new_fluid_opensles_audio_driver(fluid_settings_t* settings, fluid_synth_t* synth)
{
    return new_fluid_opensles_audio_driver2 (settings,
                                             (fluid_audio_func_t) fluid_synth_process,
                                             (void*) synth);
}

/*
 * new_fluid_opensles_audio_driver2
 */
fluid_audio_driver_t*
new_fluid_opensles_audio_driver2(fluid_settings_t* settings, fluid_audio_func_t func, void* data)
{
    SLresult result;
    fluid_opensles_audio_driver_t* dev;
    double sample_rate;
    int period_size;
    int realtime_prio = 0;
    int err;
    int is_sample_format_float;
    int use_callback_mode;

    fluid_synth_t* synth = (fluid_synth_t*) data;

    dev = FLUID_NEW(fluid_opensles_audio_driver_t);
    if (dev == NULL) {
        FLUID_LOG(FLUID_ERR, "Out of memory");
        return NULL;
    }

    FLUID_MEMSET(dev, 0, sizeof(fluid_opensles_audio_driver_t));

    fluid_settings_getint(settings, "audio.period-size", &period_size);
    fluid_settings_getnum(settings, "synth.sample-rate", &sample_rate);
    fluid_settings_getint(settings, "audio.realtime-prio", &realtime_prio);
    is_sample_format_float = fluid_settings_str_equal (settings, "audio.sample-format", "float");
    fluid_settings_getint(settings, "audio.opensles.use-callback-mode", &use_callback_mode);

    dev->data = synth;
    dev->use_callback_mode = use_callback_mode;
    dev->is_sample_format_float = is_sample_format_float;
    dev->buffer_size = period_size;
    dev->sample_rate = sample_rate;
    dev->cont = 1;

    result = slCreateEngine (&(dev->engine), 0, NULL, 0, NULL, NULL);

    if (!dev->engine)
    {
        FLUID_LOG(FLUID_ERR, "Failed to create OpenSLES connection");
        goto error_recovery;
    }
    result = (*dev->engine)->Realize (dev->engine, SL_BOOLEAN_FALSE);
    if (result != 0) goto error_recovery;

    SLEngineItf engine_interface;
    result = (*dev->engine)->GetInterface (dev->engine, SL_IID_ENGINE, &engine_interface);
    if (result != 0) goto error_recovery;

    result = (*engine_interface)->CreateOutputMix (engine_interface, &dev->output_mix_object, 0, 0, 0);
    if (result != 0) goto error_recovery;

    result = (*dev->output_mix_object)->Realize (dev->output_mix_object, SL_BOOLEAN_FALSE);
    if (result != 0) goto error_recovery;

    SLDataLocator_AndroidSimpleBufferQueue loc_buffer_queue = {
            SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE,
            2 /* number of buffers */
    };
    SLDataFormat_PCM format_pcm = {
            SL_DATAFORMAT_PCM,
            NUM_CHANNELS,
            ((SLuint32) sample_rate) * 1000,
            SL_PCMSAMPLEFORMAT_FIXED_16,
            SL_PCMSAMPLEFORMAT_FIXED_16,
            SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT,
            SL_BYTEORDER_LITTLEENDIAN
    };
    SLDataSource audio_src = {
            &loc_buffer_queue,
            &format_pcm
    };

    SLDataLocator_OutputMix loc_outmix = {
            SL_DATALOCATOR_OUTPUTMIX,
            dev->output_mix_object
    };
    SLDataSink audio_sink = {&loc_outmix, NULL};

    const SLInterfaceID ids1[] = {SL_IID_ANDROIDSIMPLEBUFFERQUEUE};
    const SLboolean req1[] = {SL_BOOLEAN_TRUE};
    result = (*engine_interface)->CreateAudioPlayer (engine_interface,
                                                     &(dev->audio_player), &audio_src, &audio_sink, 1, ids1, req1);
    if (result != 0) goto error_recovery;

    result = (*dev->audio_player)->Realize (dev->audio_player,SL_BOOLEAN_FALSE);
    if (result != 0) goto error_recovery;

    result = (*dev->audio_player)->GetInterface (dev->audio_player,
                                                 SL_IID_PLAY, &(dev->audio_player_interface));
    if (result != 0) goto error_recovery;

    result = (*dev->audio_player)->GetInterface(dev->audio_player,
                                                SL_IID_ANDROIDSIMPLEBUFFERQUEUE, &(dev->player_buffer_queue_interface));
    if (result != 0) goto error_recovery;

    if (dev->use_callback_mode) {

        if (dev->is_sample_format_float)
            dev->float_buffer = FLUID_ARRAY(float, dev->buffer_size * NUM_CHANNELS);
        else
            dev->short_buffer = FLUID_ARRAY(short, dev->buffer_size * NUM_CHANNELS);
        if (dev->float_buffer == NULL && dev->short_buffer == NULL)
        {
            FLUID_LOG(FLUID_ERR, "Out of memory.");
            return NULL;
        }

        if (dev->callback) {

            if (dev->is_sample_format_float)
                dev->float_callback_buffer_l = FLUID_ARRAY(float, dev->buffer_size);
            else
                dev->short_callback_buffer_l = FLUID_ARRAY(short, dev->buffer_size);
            if (dev->float_callback_buffer_l == NULL && dev->short_callback_buffer_l == NULL)
            {
                FLUID_LOG(FLUID_ERR, "Out of memory.");
                return NULL;
            }

            if (dev->is_sample_format_float)
                dev->float_callback_buffer_r = FLUID_ARRAY(float, dev->buffer_size);
            else
                dev->short_callback_buffer_r = FLUID_ARRAY(short, dev->buffer_size);
            if (dev->float_callback_buffer_r == NULL && dev->short_callback_buffer_r == NULL)
            {
                FLUID_LOG(FLUID_ERR, "Out of memory.");
                return NULL;
            }
        }

        result = (*dev->player_buffer_queue_interface)->RegisterCallback(dev->player_buffer_queue_interface, fluid_opensles_callback, dev);
        if (result != 0) goto error_recovery;

        if (dev->is_sample_format_float)
            (*dev->player_buffer_queue_interface)->Enqueue(dev->player_buffer_queue_interface, dev->float_buffer, dev->buffer_size * NUM_CHANNELS * sizeof(float));
        else
            (*dev->player_buffer_queue_interface)->Enqueue(dev->player_buffer_queue_interface, dev->short_buffer, dev->buffer_size * NUM_CHANNELS * sizeof(short));

        (*dev->audio_player_interface)->SetCallbackEventsMask(dev->audio_player_interface, SL_PLAYEVENT_HEADATEND);
        result = (*dev->audio_player_interface)->SetPlayState(dev->audio_player_interface, SL_PLAYSTATE_PLAYING);
        if (result != 0) goto error_recovery;

    } else { /* non-callback mode */

        result = (*dev->audio_player_interface)->SetPlayState(dev->audio_player_interface, SL_PLAYSTATE_PLAYING);
        if (result != 0) goto error_recovery;

        /* Create the audio thread */
        dev->thread = new_fluid_thread ("opensles-audio", fluid_opensles_audio_run,
                                        dev, realtime_prio, FALSE);
        if (!dev->thread)
            goto error_recovery;
    }

    FLUID_LOG(FLUID_INFO, "Using OpenSLES driver");

    return (fluid_audio_driver_t*) dev;

    error_recovery:
    delete_fluid_opensles_audio_driver((fluid_audio_driver_t*) dev);
    return NULL;
}

void delete_fluid_opensles_audio_driver(fluid_audio_driver_t* p)
{
    fluid_opensles_audio_driver_t* dev = (fluid_opensles_audio_driver_t*) p;

    if (dev == NULL) {
        return;
    }

    dev->cont = 0;

    if (!dev->use_callback_mode) {
        if (dev->thread)
            fluid_thread_join (dev->thread);
    }

    if (dev->audio_player)
        (*dev->audio_player)->Destroy (dev->audio_player);
    if (dev->output_mix_object)
        (*dev->output_mix_object)->Destroy (dev->output_mix_object);
    if (dev->engine)
        (*dev->engine)->Destroy (dev->engine);

    if (dev->use_callback_mode) {
        if (dev->is_sample_format_float) {
            if (dev->float_callback_buffer_l)
                FLUID_FREE(dev->float_callback_buffer_l);
            if (dev->float_callback_buffer_r)
                FLUID_FREE(dev->float_callback_buffer_r);
            if (dev->float_buffer)
                FLUID_FREE(dev->float_buffer);
        } else {
            if (dev->short_callback_buffer_l)
                FLUID_FREE(dev->short_callback_buffer_l);
            if (dev->short_callback_buffer_r)
                FLUID_FREE(dev->short_callback_buffer_r);
            if (dev->short_buffer)
                FLUID_FREE(dev->short_buffer);
        }
    }

    FLUID_FREE(dev);

    return;
}

void fluid_opensles_adjust_latency(fluid_opensles_audio_driver_t* dev)
{
    struct timespec ts;
    long current_time, wait_in_theory;

    wait_in_theory = 1000000 * dev->buffer_size / dev->sample_rate;

    /* compute delta time and update 'next expected enqueue' time */
    clock_gettime(CLOCK_REALTIME, &ts);
    current_time = ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
    long time_delta = dev->next_expected_enqueue_time == 0 ? 0 : dev->next_expected_enqueue_time - current_time;
    if (time_delta == 0)
        dev->next_expected_enqueue_time = current_time + wait_in_theory;
    else
        dev->next_expected_enqueue_time += wait_in_theory;
    /* take some sleep only if it's running ahead */
    if (time_delta > 0)
        usleep (time_delta);
}

void fluid_opensles_callback(SLAndroidSimpleBufferQueueItf caller, void *pContext)
{
    fluid_opensles_audio_driver_t* dev = (fluid_opensles_audio_driver_t*) pContext;

    short *short_buf = dev->short_buffer;
    float *float_buf = dev->float_buffer;
    int buffer_size;
    int i, k;
    //int err;
    float *float_callback_buffers[2];
    SLresult result;

    buffer_size = dev->buffer_size;

    if (dev->callback)
    {
        if (dev->is_sample_format_float) {
            float* left = dev->float_callback_buffer_l;
            float* right = dev->float_callback_buffer_r;
            float_callback_buffers [0] = left;
            float_callback_buffers [1] = right;

            (*dev->callback)(dev->data, buffer_size, 0, NULL, 2, float_callback_buffers);

            for (i = 0, k = 0; i < buffer_size; i++) {
                float_buf[k++] = left[i];
                float_buf[k++] = right[i];
            }
        } else {
            FLUID_LOG(FLUID_ERR, "callback is not supported when audio.sample-format is '16bits'.");
        }
    }
    else {
        if (dev->is_sample_format_float)
            fluid_synth_write_float(dev->data, buffer_size, float_buf, 0, 2, float_buf, 1, 2);
        else
            fluid_synth_write_s16(dev->data, buffer_size, short_buf, 0, 2, short_buf, 1, 2);
    }

    if (dev->is_sample_format_float)
        result = (*caller)->Enqueue (
                dev->player_buffer_queue_interface, float_buf, buffer_size * sizeof (float) * NUM_CHANNELS);
    else
        result = (*caller)->Enqueue (
                dev->player_buffer_queue_interface, short_buf, buffer_size * sizeof (short) * NUM_CHANNELS);
    if (result != 0) {
        //err = result;
        /* Do not simply break at just one single insufficient buffer. Go on. */
    }
}

/* Thread without audio callback, more efficient */
static void*
fluid_opensles_audio_run(void* d)
{
    fluid_opensles_audio_driver_t* dev = (fluid_opensles_audio_driver_t*) d;
    short *short_buf = NULL;
    float *float_buf = NULL;
    int buffer_size;
    //int err;
    SLresult result;

    buffer_size = dev->buffer_size;

    /* FIXME - Probably shouldn't alloc in run() */
    if (dev->is_sample_format_float)
        float_buf = FLUID_ARRAY(float, buffer_size * NUM_CHANNELS);
    else
        short_buf = FLUID_ARRAY(short, buffer_size * NUM_CHANNELS);

    if (short_buf == NULL && float_buf == NULL)
    {
        FLUID_LOG(FLUID_ERR, "Out of memory.");
        return NULL;
    }

    int cnt = 0;

    while (dev->cont)
    {
        fluid_opensles_adjust_latency (dev);

        /* it seems that the synth keeps emitting synthesized buffers even if there is no sound. So keep feeding... */
        if (dev->is_sample_format_float)
            fluid_synth_write_float(dev->data, buffer_size, float_buf, 0, 2, float_buf, 1, 2);
        else
            fluid_synth_write_s16(dev->data, buffer_size, short_buf, 0, 2, short_buf, 1, 2);

        if (dev->is_sample_format_float)
            result = (*dev->player_buffer_queue_interface)->Enqueue (
                    dev->player_buffer_queue_interface, float_buf, buffer_size * sizeof (float) * NUM_CHANNELS);
        else
            result = (*dev->player_buffer_queue_interface)->Enqueue (
                    dev->player_buffer_queue_interface, short_buf, buffer_size * sizeof (short) * NUM_CHANNELS);
        if (result != 0) {
            //err = result;
            /* Do not simply break at just one single insufficient buffer. Go on. */
        }
    }	/* while (dev->cont) */

    if (dev->is_sample_format_float)
        FLUID_FREE(float_buf);
    else
        FLUID_FREE(short_buf);

    return FLUID_THREAD_RETURN_VALUE;
}
