# 8: 703.306,714.268,749.351
# 16: 349.789,354.393,352.843
# 32: 179.886,185.757,190.902
# 64: 110.773,110.425,111.290

import numpy as np
from matplotlib import pyplot as plt

x = [8, 16, 32, 64]
run8 = np.mean([703.306,714.268,749.351])
run16 = np.mean([349.789,354.393,352.843])
run32 = np.mean([179.886,185.757,190.902])
run64 = np.mean([110.773,110.425,111.290])

y = [run8, run16, run32, run64]

plt.plot(x, y)
plt.savefig("fig.png")

