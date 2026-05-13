# Phase 0.5 — Hardware Validation Run Summary

- **date**: 2026-05-12
- **backend**: rocm
- **endpoint**: http://127.0.0.1:8080
- **models**: qwen-2.5-7b
- **slots**: 4
- **grammars**: json_schema
- **steady_rps**: 1.0
- **steady_duration_s**: 60.0
- **burst_requests**: 50
- **gpu_power_cap_watts**: 237
- **inter_cell_cooling_s**: 0.0
- **dry_run**: False
- **single_cell**: True

## Per-cell aggregates

| model       | grammar     |   slots | phase   |   N |   p50 ms |   p95 ms |   p99 ms |   decode tok/s |   prefill tok/s | adherence   |   VRAM idle MB |   VRAM loaded MB |   edge °C |   junction °C |
|:------------|:------------|--------:|:--------|----:|---------:|---------:|---------:|---------------:|----------------:|:------------|---------------:|-----------------:|----------:|--------------:|
| qwen-2.5-7b | json_schema |       4 | steady  |  60 |     2225 |     4190 |     5098 |           44.6 |             267 | 100.0%      |           5316 |             5357 |        56 |            73 |
| qwen-2.5-7b | json_schema |       4 | burst   |  50 |     4636 |     5750 |     6214 |           41.1 |              38 | 100.0%      |           5316 |             5357 |        56 |            73 |
