# A minimal Tab5 OS Python app: an app bar, a counter and a Grove I2C scan.
# Hardware is reached through the t5 module only (docs/sdk.md, "Python").
import t5

screen = t5.ui.screen("Hello")          # the back arrow closes the app
label = screen.label("Hello, Tab5", role="h5")
taps = 0


def tapped(button):
    global taps
    taps += 1
    label.set_text("Tapped %d times" % taps)


screen.button("Tap me", fn=tapped)
found = screen.label("Scanning Grove...")
with t5.port.open("grove", mode="i2c", hz=100_000) as grove:
    addrs = grove.scan()
found.set_text("Grove I2C: " + (", ".join(hex(a) for a in addrs) or "nothing found"))
print("hello: grove devices", addrs)

while t5.app.running():
    t5.sleep_ms(250)
