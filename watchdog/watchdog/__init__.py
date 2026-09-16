"""AutoTrader watchdog / kill switch.

Standalone: Python stdlib only, its own Alpaca API key, no imports from the
trading system. Its action path talks to Alpaca REST directly; the bus is
only used, best effort, to announce control.halt afterwards.
"""

__version__ = "0.1.0"
