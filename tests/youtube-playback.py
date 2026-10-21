"""Measure live media controls through typed DevTools commands and serial results."""
import json
import re
import time


def run(key, healthy, snapshot, build):
    punctuation = {
        ' ': ('spc',), '"': ('shift', 'apostrophe'), '.': ('dot',),
        ',': ('comma',), '(': ('shift', '9'), ')': ('shift', '0'),
        '_': ('shift', 'minus'), '?': ('shift', 'slash'),
        '=': ('equal',), ';': ('semicolon',), '[': ('bracket_left',),
        ']': ('bracket_right',), '{': ('shift', 'bracket_left'),
        '}': ('shift', 'bracket_right'), '-': ('minus',),
    }
    results = {'samples': [], 'checks': {}, 'av_sync_verified': False,
               'interval_visibility': 'video tab during waits; inspector during commands'}
    destination = build / 'youtube-playback-results.json'

    def save():
        destination.write_text(json.dumps(results, indent=2) + '\n')

    def type_command(expression):
        for index, character in enumerate(expression):
            results['input_position'] = {'index': index, 'character': character,
                                         'length': len(expression)}
            healthy()
            key(*punctuation.get(character,
                ('shift', character.lower()) if character.isupper() else (character,)))
        key('ret')

    def wait(seconds):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            healthy()
            time.sleep(.2)

    def sample(identifier, prefix=''):
        offset = len(healthy())
        started = time.monotonic()
        type_command(prefix + f's({identifier})')
        deadline = time.monotonic() + 45
        while time.monotonic() < deadline:
            for match in re.finditer(r'YTPROBE (\[[^\r\n]*\])', healthy()[offset:]):
                values = json.loads(match[1])
                if values[0] != identifier:
                    continue
                record = dict(zip(('identifier', 'clock_ms', 'media_seconds',
                    'paused', 'seeking', 'ready_state', 'error', 'decoded_frames',
                    'playback_rate', 'visibility'), values))
                record['command_seconds'] = time.monotonic() - started
                results['samples'].append(record)
                save()
                print('YOUTUBE PLAYBACK SAMPLE: ' + json.dumps(record), flush=True)
                return record
            time.sleep(.2)
        snapshot('youtube-playback-missing-' + str(identifier))
        raise AssertionError(f'Live media result {identifier} did not arrive')

    def visible_interval(seconds):
        key('ctrl', '1')
        wait(seconds)
        snapshot('youtube-playback-visible')
        key('ctrl', '2')
        wait(2)

    def progression(first, last):
        elapsed = (last['clock_ms'] - first['clock_ms']) / 1000
        return {'elapsed_seconds': elapsed,
                'media_seconds': last['media_seconds'] - first['media_seconds'],
                'ratio': (last['media_seconds'] - first['media_seconds']) / elapsed}

    def measure_visible_playback():
        offset = len(healthy())
        # Both endpoints run in the visible video page, without a tab switch or
        # another Inspector command inside the measured interval.
        type_command('function w(){document.removeEventListener("visibilitychange",w);'
            'var a=performance.now(),b=v.currentTime,f=v.getVideoPlaybackQuality().totalVideoFrames,h=false;'
            'function c(){h=true}document.addEventListener("visibilitychange",c);'
            'setTimeout(function(){document.removeEventListener("visibilitychange",c);'
            'console.log("YTVISIBLE",JSON.stringify([performance.now()-a,v.currentTime-b,'
            'v.getVideoPlaybackQuality().totalVideoFrames-f,h,v.paused,v.seeking,v.error?.code,'
            'document.visibilityState]))},20000)}document.addEventListener("visibilitychange",w);'
            'console.log("YTVISIBLE ARMED",document.visibilityState)')
        # Console evaluation can lag behind the native Enter key. Wait for the
        # listener to exist before switching tabs and generating its event.
        deadline = time.monotonic() + 45
        while 'YTVISIBLE ARMED hidden' not in healthy()[offset:]:
            if time.monotonic() >= deadline:
                snapshot('youtube-visible-installation-timeout')
                raise AssertionError('Visible playback listener was not installed in the hidden video page')
            time.sleep(.2)
        results['visible_listener_armed'] = True
        save()
        key('ctrl', '1')
        deadline = time.monotonic() + 90
        while time.monotonic() < deadline:
            match = re.search(r'YTVISIBLE (\[[^\r\n]*\])', healthy()[offset:])
            if match:
                values = json.loads(match[1])
                record = dict(zip(('elapsed_ms', 'advanced_seconds', 'decoded_frames',
                    'visibility_changed', 'paused', 'seeking', 'error', 'visibility'), values))
                record['ratio'] = record['advanced_seconds'] / (record['elapsed_ms'] / 1000)
                results['visible_playback'] = record
                save()
                snapshot('youtube-visible-measurement')
                key('ctrl', '2')
                wait(2)
                return record
            time.sleep(.2)
        snapshot('youtube-visible-timeout')
        key('ctrl', '2')
        snapshot('youtube-visible-timeout-console')
        raise AssertionError('Visible playback measurement did not arrive after listener acknowledgement')

    try:
        # The console receives ordinary keyboard input; no debugger injection is used.
        type_command('var v=document.querySelector("video");function s(n){console.log("YTPROBE",JSON.stringify([n,performance.now(),v.currentTime,v.paused,v.seeking,v.readyState,v.error?.code,v.getVideoPlaybackQuality().totalVideoFrames,v.playbackRate,document.visibilityState]))}')
        snapshot('youtube-playback-probe-installed')
        first = sample(0)
        visible_interval(15)
        second = sample(1)
        results['playing_progress'] = progression(first, second)
        sample(2, 'v.pause();')
        wait(2)
        paused = sample(3)
        visible_interval(5)
        paused_later = sample(4)
        results['paused_progress'] = progression(paused, paused_later)
        sample(5, 'v.play();')
        wait(3)
        resumed = sample(6)
        visible_interval(15)
        resumed_later = sample(7)
        results['resumed_progress'] = progression(resumed, resumed_later)
        sample(8, 'v.currentTime=120;')
        settled = None
        for identifier in range(9, 15):
            visible_interval(5)
            settled = sample(identifier)
            if not settled['seeking'] and settled['ready_state'] >= 2:
                break
        visible_interval(10)
        after_seek = sample(15)
        results['seek_progress'] = progression(settled, after_seek)
        visible = measure_visible_playback()
        results['checks'] = {
            'no_media_error': all(item['error'] is None for item in results['samples']),
            'playing_advances': results['playing_progress']['media_seconds'] > 1,
            'video_decodes': second['decoded_frames'] > first['decoded_frames'],
            'playing_realtime': .9 <= results['playing_progress']['ratio'] <= 1.1,
            'pause_holds': paused['paused'] and paused_later['paused'] and
                           abs(results['paused_progress']['media_seconds']) < .1,
            'resume_advances': not resumed_later['paused'] and
                               results['resumed_progress']['media_seconds'] > 1,
            'resume_realtime': .9 <= results['resumed_progress']['ratio'] <= 1.1,
            'seek_completes': not settled['seeking'] and settled['ready_state'] >= 2 and
                              119.5 <= settled['media_seconds'] < 180,
            'seek_advances': results['seek_progress']['media_seconds'] > 1,
            'visible_interval_valid': visible['elapsed_ms'] >= 20000 and
                not visible['visibility_changed'] and visible['visibility'] == 'visible' and
                not visible['paused'] and not visible['seeking'] and visible['error'] is None,
            'visible_video_decodes': visible['decoded_frames'] > 0,
            'visible_realtime': .9 <= visible['ratio'] <= 1.1,
        }
        save()
        snapshot('youtube-playback-final')
        if not all(results['checks'].values()):
            raise AssertionError('Live media checks failed: ' + json.dumps(results['checks']))
        print('PASS: live media progression, pause, resume and seek; A/V sync unverified', flush=True)
    except Exception as error:
        results['failure'] = str(error)
        save()
        raise
