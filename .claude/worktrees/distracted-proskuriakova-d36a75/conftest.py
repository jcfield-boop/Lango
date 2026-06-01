"""conftest.py — shared pytest fixtures for Langoustine device tests."""

def pytest_addoption(parser):
    parser.addoption(
        "--ip",
        default="192.168.0.44",
        help="Device IP address (default: 192.168.0.44)",
    )
