import numpy as np
import pandas as pd

# Updated calibration data
cal = pd.DataFrame([
    [1009.000, 3444, 1147, 3458.63, 1151.87],
    [980.000, 3409, 1133, 3413.50, 1134.50],
    [0.000, 1705, 579, 1693.63, 575.14],
], columns=["weight_g", "raw", "mv", "avg_raw", "avg_mv"])

# Linear fit: weight_g = slope * avg_mv + intercept
slope, intercept = np.polyfit(cal["avg_mv"], cal["weight_g"], 1)
print(f"slope = {slope:.6f}, intercept = {intercept:.2f}")
