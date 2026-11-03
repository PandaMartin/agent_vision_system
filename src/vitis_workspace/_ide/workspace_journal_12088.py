# 2026-10-19T17:21:46.531800300
import vitis

client = vitis.create_client()
client.set_workspace(path="vitis")

platform = client.create_platform_component(name = "FreeRtos_Platform",hw_design = "$COMPONENT_LOCATION/../../system_wrapper.xsa",os = "standalone",cpu = "ps7_cortexa9_0",domain_name = "standalone_ps7_cortexa9_0",compiler = "gcc")

platform = client.create_platform_component(name = "FreeRtos_Platform",hw_design = "$COMPONENT_LOCATION/../../system_wrapper.xsa",os = "freertos",cpu = "ps7_cortexa9_0",domain_name = "freertos_ps7_cortexa9_0")

platform = client.create_platform_component(name = "standalone",hw_design = "$COMPONENT_LOCATION/../../system_wrapper.xsa",os = "standalone",cpu = "ps7_cortexa9_0",domain_name = "standalone_ps7_cortexa9_0",compiler = "gcc")

platform = client.get_component(name="standalone")
domain = platform.get_domain(name="standalone_ps7_cortexa9_0")

status = domain.set_lib(lib_name="lwip220", path="D:\software\AMDDesign\2025.2\Vitis\data\embeddedsw\ThirdParty\sw_services\lwip220_v1_3")

status = platform.build()

status = domain.set_config(option = "lib", param = "XILTIMER_en_interval_timer", value = "true", lib_name="xiltimer")

status = domain.set_config(option = "lib", param = "lwip220_dhcp", value = "true", lib_name="lwip220")

status = domain.set_config(option = "lib", param = "lwip220_lwip_dhcp_does_acd_check", value = "true", lib_name="lwip220")

status = domain.set_config(option = "lib", param = "lwip220_pbuf_pool_size", value = "2048", lib_name="lwip220")

status = platform.build()

comp = client.create_app_component(name="lwip_echo_server",platform = "$COMPONENT_LOCATION/../standalone/export/standalone/standalone.xpfm",domain = "standalone_ps7_cortexa9_0",template = "lwip_echo_server")

status = platform.build()

comp = client.get_component(name="lwip_echo_server")
comp.build()

vitis.dispose()

