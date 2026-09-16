# Fama-French 3 factors (daily)

* Source: https://mba.tuck.dartmouth.edu/pages/faculty/ken.french/ftp/F-F_Research_Data_Factors_daily_CSV.zip
* Retrieved: 2026-09-16
* sha256 of the downloaded zip: `1916d331c2c51d2aee3d00215897d2b8e5995cb387f1f4569ba46bff5fb049a8`
* Rows: 26296, 1926-07-01 to 2026-07-31
* Units: converted from percent to decimals by scripts/fetch_ff_factors.py

The library publishes with a lag of roughly one to two months. Residual momentum skips the
most recent month, so the lag does not block a live monthly rebalance, but check the last
covered date before every live formation.
