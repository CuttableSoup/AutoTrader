"""AutoTrader Python sidecars.

Everything here speaks JetStream through :mod:`autotrader.bus` and validates
every message against ``schemas/`` through :mod:`autotrader.schemas`.
"""
import logging

__version__ = "0.1.0"

# httpx logs every request URL at INFO. Several vendors (FMP, Finnhub, Nasdaq)
# authenticate with a query-string key, so those URLs carry live credentials
# into var/log and into the 2-year topic archive. Keep the HTTP client quiet
# unless someone deliberately turns it back up.
logging.getLogger("httpx").setLevel(logging.WARNING)
logging.getLogger("httpcore").setLevel(logging.WARNING)
