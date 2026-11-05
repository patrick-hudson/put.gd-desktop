"""Expected failure checks for the bounded recording reproducer (no I/O except archive)."""
def assert_expected_failure(report, archive):
    assert report['baseline'] or report['exhaust'], 'not an expected-failure scenario'
    after = report['after']
    assert after.get('ok') is False, after
    assert after['error']['code'] == 'recording_unavailable', after
    operation = report['operationBeforeUpload']
    assert operation['state'] == 'failed', operation
    assert operation['items'] == [], operation
    error = operation['error']
    if report['baseline']:
        assert error['code'] == 'recording_failed', error
        assert 'No space left' in error['message'], error
        # Baseline has neither the candidate reason nor finalized contract.
    else:
        assert error['code'] == 'recording_disk_full', error
        assert error['details']['reason'] == 'disk_space', error
        assert error['details']['finalized'] is False, error
    assert not list(archive.rglob('*')), 'automatic publication'
