import numpy as np
import pandas as pd
import matplotlib.pyplot as plt

# User's new measurement series ONLY
cal = pd.DataFrame([
    [0.000, 1488, 512, 1488.07, 512.87],
    [130.000, 1617, 554, 1617.27, 554.23],
    [100.000, 1586, 544, 1586.03, 544.20],
    [475.000, 2061, 697, 2061.93, 697.17],
    [570.000, 2206, 744, 2206.00, 744.93],
    [790.000, 2481, 834, 2481.17, 834.30],
    [1010.000, 2732, 914, 2732.70, 914.80],
    [985.000, 2823, 944, 2823.90, 944.67]
], columns=["weight_g", "raw", "mv", "avg_raw", "avg_mv"])



[
    [570.000, 1301, 451, 1301.57, 451.77],
    [1010.000, 1343, 465, 1343.67, 465.77],
    [985.000, 1993, 677, 1993.70, 677.23],
    [0.000, 1365, 473, 1365.17, 473.30],
    [95.000, 1425, 493, 1425.13, 493.27],
]
# Linear fit: weight_g = slope * avg_mv + intercept
slope, intercept = np.polyfit(cal["avg_mv"], cal["weight_g"], 1)
print(f"slope = {slope:.6f}, intercept = {intercept:.2f}")

# Calculate TARE_MV for empty spool (e.g., 247g)
empty_spool_weight = 247
TARE_MV = (empty_spool_weight - intercept) / slope
print(f"TARE_MV for {empty_spool_weight}g spool: {TARE_MV:.2f} mV")

# Fit analysis
predicted = slope * cal["avg_mv"] + intercept
residuals = cal["weight_g"] - predicted

r2 = 1 - np.sum(residuals**2) / np.sum((cal["weight_g"] - cal["weight_g"].mean())**2)
print(f"R^2 = {r2:.4f}")
print("Residuals:")
for i, res in enumerate(residuals):
    print(f"  Point {i+1}: {res:.2f} g")

# Plot
plt.figure(figsize=(8,5))
plt.scatter(cal["avg_mv"], cal["weight_g"], label="Measured", color="blue")
plt.plot(cal["avg_mv"], predicted, label="Fit", color="red")
plt.xlabel("avg_mv")
plt.ylabel("weight_g")
plt.title("Calibration Fit: weight_g vs avg_mv (New Only)")
plt.legend()
plt.grid(True)
plt.tight_layout()
plt.show()
