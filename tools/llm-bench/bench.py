#!/usr/bin/env -S uv run --project tools/llm-bench
"""Phase 0.5 hardware-validation bench entrypoint."""

import click


@click.command()
@click.option("--endpoint", default="http://127.0.0.1:8080", help="llama-server base URL")
def main(endpoint: str) -> None:
    """Run the Phase 0.5 characterization matrix."""
    click.echo(f"endpoint={endpoint}  (scaffolding stub; nothing wired yet)")


if __name__ == "__main__":
    main()
