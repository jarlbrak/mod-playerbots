from llmbench.vram import parse_vram_bytes, sample_vram


def test_parse_vram_bytes_strips_whitespace_and_newline():
    assert parse_vram_bytes("12345678\n") == 12345678


def test_parse_vram_bytes_handles_zero():
    assert parse_vram_bytes("0") == 0


def test_parse_vram_bytes_rejects_negative():
    import pytest

    with pytest.raises(ValueError):
        parse_vram_bytes("-1")


def test_parse_vram_bytes_rejects_nonint():
    import pytest

    with pytest.raises(ValueError):
        parse_vram_bytes("not a number")


def test_sample_vram_reads_from_tmp_path(tmp_path):
    card = tmp_path / "card0" / "device"
    card.mkdir(parents=True)
    (card / "mem_info_vram_used").write_text("536870912\n")
    (card / "mem_info_vram_total").write_text("17179869184\n")

    used_mb, total_mb = sample_vram(str(tmp_path / "card0" / "device"))
    assert used_mb == 512
    assert total_mb == 16384
