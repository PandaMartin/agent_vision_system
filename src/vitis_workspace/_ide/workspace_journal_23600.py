# 2026-10-20T18:57:39.016094800
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

domain = platform.get_domain(name="freertos_ps7_cortexa9_0")

status = domain.set_config(option = "lib", param = "lwip220_temac_phy_link_speed", value = "CONFIG_LINKSPEED100", lib_name="lwip220")

status = platform.build()

status = domain.set_config(option = "lib", param = "lwip220_tcp_wnd", value = "11680", lib_name="lwip220")

status = platform.build()

status = platform.build()

comp.build()

status = domain.set_config(option = "lib", param = "lwip220_temac_phy_link_speed", value = "CONFIG_LINKSPEED_AUTODETECT", lib_name="lwip220")

status = platform.build()

comp.build()

status = platform.build()

comp.build()

status = platform.build()

comp.build()

status = platform.build()

comp.build()

vitis.dispose()

