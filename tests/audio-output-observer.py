"""Observe QEMU WAV writes during the fixture's blocked JavaScript interval."""
import json
import struct
import time


def annotate_pcm(samples, raw):
    if (raw[:4] != b'RIFF' or raw[8:16] != b'WAVEfmt ' or raw[36:40] != b'data' or
            struct.unpack_from('<HHIIHH', raw, 20) != (1, 2, 48000, 192000, 4, 16)):
        raise AssertionError('Unexpected PCM capture format')
    previous = 44
    signal_bytes = 0
    for sample in samples:
        end = sample['bytes']
        if end < previous or end > len(raw) or (end - 44) % 4:
            raise AssertionError('The PCM observation is outside the captured frame range')
        for left, right in struct.iter_unpack('<hh', raw[previous:end]):
            if abs(left) > 100 or abs(right) > 100:
                signal_bytes += 4
        sample['signal_bytes'] = signal_bytes
        previous = end


def evaluate(samples, begin, end):
    # Exclude marker detection and backend buffering at both interval edges.
    interior = [sample for sample in samples if begin + .1 <= sample['seconds'] <= end - .1]
    if len(interior) < 2:
        raise AssertionError('No observable interval between the JavaScript markers')
    elapsed = interior[-1]['seconds'] - interior[0]['seconds']
    byte_count = interior[-1]['bytes'] - interior[0]['bytes']
    maximum_sample_gap = maximum_write_gap = maximum_signal_gap = 0
    last_write = interior[0]['seconds']
    last_signal = last_write
    for previous, sample in zip(interior, interior[1:]):
        if sample['bytes'] < previous['bytes']:
            raise AssertionError('The audio capture shrank during observation')
        maximum_sample_gap = max(maximum_sample_gap, sample['seconds'] - previous['seconds'])
        maximum_write_gap = max(maximum_write_gap, sample['seconds'] - last_write)
        maximum_signal_gap = max(maximum_signal_gap, sample['seconds'] - last_signal)
        if sample['bytes'] > previous['bytes']:
            last_write = sample['seconds']
        if sample['signal_bytes'] > previous['signal_bytes']:
            last_signal = sample['seconds']
    ratio = byte_count / (192000 * elapsed) if elapsed else 0
    signal_ratio = (interior[-1]['signal_bytes'] - interior[0]['signal_bytes']) / (192000 * elapsed) if elapsed else 0
    return {
        'interior_seconds': elapsed,
        'written_bytes': byte_count,
        'output_to_wall_clock_ratio': ratio,
        'maximum_sample_gap_seconds': maximum_sample_gap,
        'maximum_write_gap_seconds': maximum_write_gap,
        'signal_to_wall_clock_ratio': signal_ratio,
        'maximum_signal_gap_seconds': maximum_signal_gap,
        'checks': {
            'interval_at_least_one_second': elapsed >= 1,
            'observer_sample_gap_bounded': maximum_sample_gap <= .08,
            'pcm_output_realtime': .9 <= ratio <= 1.1,
            'pcm_write_gap_bounded': maximum_write_gap <= .15,
            'signal_output_realtime': .9 <= signal_ratio <= 1.1,
            'signal_gap_bounded': maximum_signal_gap <= .15,
        },
    }


def run(healthy, capture, destination):
    started = time.monotonic()
    report = {'samples': [], 'begin_seconds': None, 'end_seconds': None,
              'physical_output_verified': False, 'av_sync_verified': False}
    try:
        while time.monotonic() - started < 30:
            content = healthy()
            sample = {'seconds': time.monotonic() - started,
                      'bytes': capture.stat().st_size}
            report['samples'].append(sample)
            if report['begin_seconds'] is None and 'MSE RESERVOIR BLOCK BEGIN' in content:
                report['begin_seconds'] = sample['seconds']
            if 'MSE RESERVOIR BLOCK END' in content:
                report['end_seconds'] = sample['seconds']
                break
            time.sleep(.02)
        if report['begin_seconds'] is None or report['end_seconds'] is None:
            raise AssertionError('The blocked JavaScript interval was not observed')
        annotate_pcm(report['samples'], capture.read_bytes())
        report.update(evaluate(report['samples'], report['begin_seconds'], report['end_seconds']))
        if not all(report['checks'].values()):
            raise AssertionError('Live audio output checks failed: ' + json.dumps(report['checks']))
        print('PASS: host-timed PCM writes during the blocked JavaScript interval', flush=True)
    except Exception as error:
        report['failure'] = str(error)
        raise
    finally:
        destination.write_text(json.dumps(report, indent=2) + '\n')
