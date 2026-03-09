import numpy as np
import pandas as pd
import matplotlib.pyplot as plt

# Calibration data from new measurements (including empty spool for tare)
cal = pd.DataFrame([
	[96.000,1660,566,1661.50,566.51],
	[96.000,1657,574,1658.97,574.68],
	[985.000,2445,821,2449.10,822.38],
	[985.000,2492,840,2494.83,840.96],
	[985.000,2484,838,2488.87,839.64],
	[0.000,1576,540,1573.33,539.09],
	[0.000,1569,544,1572.83,545.33],
	[0.000,1564,532,1562.63,531.54],
	[570.000,2020,685,2019.00,684.66],
	[570.000,1952,666,1956.30,667.47],
	[570.000,2068,700,2067.73,699.91],
], columns=["weight_g", "raw", "mv", "avg_raw", "avg_mv"])

# Linear fit: weight_g = slope * avg_mv + intercept
slope, intercept = np.polyfit(cal["avg_mv"], cal["weight_g"], 1)
print(f"slope = {slope:.6f}, intercept = {intercept:.2f}")

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
plt.title("Calibration Fit: weight_g vs avg_mv")
plt.legend()
plt.grid(True)
plt.tight_layout()
plt.show()
