# 2026-10-20T14:53:36.569150800
import vitis

client = vitis.create_client()
client.set_workspace(path="vitis")

platform = client.get_component(name="FreeRtos_Platform")
domain = platform.get_domain(name="freertos_ps7_cortexa9_0")

status = domain.set_lib(lib_name="xilffs", path="D:\software\AMDDesign\2025.2\Vitis\data\embeddedsw\lib\sw_services\xilffs_v5_5")

status = domain.set_lib(lib_name="lwip220", path="D:\software\AMDDesign\2025.2\Vitis\data\embeddedsw\ThirdParty\sw_services\lwip220_v1_3")

status = domain.set_config(option = "lib", param = "lwip220_api_mode", value = "SOCKET_API", lib_name="lwip220")

status = domain.set_config(option = "lib", param = "lwip220_dhcp", value = "true", lib_name="lwip220")

status = domain.set_config(option = "lib", param = "lwip220_dhcp", value = "false", lib_name="lwip220")

comp = client.create_app_component(name="lwip_echo_server_freertos",platform = "$COMPONENT_LOCATION/../FreeRtos_Platform/export/FreeRtos_Platform/FreeRtos_Platform.xpfm",domain = "freertos_ps7_cortexa9_0",template = "empty_application")

status = platform.build()

status = domain.set_config(option = "lib", param = "XILFFS_use_lfn", value = "1", lib_name="xilffs")

status = domain.set_config(option = "lib", param = "XILFFS_use_strfunc", value = "1", lib_name="xilffs")

status = platform.build()

comp = client.get_component(name="lwip_echo_server_freertos")
comp.build()

status = domain.set_config(option = "lib", param = "lwip220_temac_phy_link_speed", value = "CONFIG_LINKSPEED1000", lib_name="lwip220")

status = platform.build()

status = platform.build()

comp.build()

status = platform.build()

comp.build()

status = domain.set_config(option = "lib", param = "lwip220_temac_phy_link_speed", value = "CONFIG_LINKSPEED_AUTODETECT", lib_name="lwip220")

status = platform.build()

comp.build()

status = platform.build()

comp.build()

comp = client.create_app_component(name="cam_lcd_hdmi_servo_net",platform = "$COMPONENT_LOCATION/../FreeRtos_Platform/export/FreeRtos_Platform/FreeRtos_Platform.xpfm",domain = "freertos_ps7_cortexa9_0",template = "empty_application")

client.delete_component(name="standalone")

client.delete_component(name="lwip_echo_server")

client.delete_component(name="componentName")

vitis.dispose()

