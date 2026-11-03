# 2026-10-19T17:56:40.319247700
import vitis

client = vitis.create_client()
client.set_workspace(path="vitis")

platform = client.get_component(name="standalone")
domain = platform.get_domain(name="standalone_ps7_cortexa9_0")

status = domain.set_config(option = "lib", param = "lwip220_dhcp", value = "false", lib_name="lwip220")

status = platform.build()

status = domain.set_config(option = "lib", param = "lwip220_dhcp", value = "true", lib_name="lwip220")

status = platform.build()

comp = client.get_component(name="lwip_echo_server")
comp.build()

status = platform.build()

comp.build()

vitis.dispose()

