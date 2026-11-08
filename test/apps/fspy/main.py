# File isolation for Python apps (t5_app/fspolicy.h).
def can(path, mode):
    try:
        open(path, mode).close()
        return True
    except Exception:
        return False
d = "."   # the working directory is the app's data folder
print("fspy: PASS own" if can(d + "/t.txt", "w") else "fspy: FAIL own")
print("fspy: PASS other" if not can("/data/appdata/com.t5.test.crasher/x", "w") else "fspy: FAIL other")
print("fspy: PASS os" if not can("/data/sys/crashes.bin", "r") else "fspy: FAIL os")
print("fspy: PASS shared" if not can("/sd/Documents/fs.txt", "r") else "fspy: FAIL shared")
print("fspy: PASS sdroot" if not can("/sd/x.txt", "w") else "fspy: FAIL sdroot")
print("fspy: done")
