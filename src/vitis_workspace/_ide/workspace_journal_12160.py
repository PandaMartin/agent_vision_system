# 2026-10-23T19:37:58.236592400
import vitis

client = vitis.create_client()
client.set_workspace(path="vitis")

platform = client.get_component(name="FreeRtos_Platform")
status = platform.build()

comp = client.get_component(name="cam_lcd_hdmi_servo_net")
comp.build()

status = platform.build()

comp.build()

status = platform.build()

comp.build()

status = platform.build()

comp.build()

status = platform.build()

comp.build()

status = platform.build()

comp.build()

status = platform.build()

comp.build()

status = platform.build()

comp.build()

status = platform.build()

comp.build()

status = comp.clean()

status = platform.build()

comp.build()

vitis.dispose()

