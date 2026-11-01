# Pre-upload: erase the app partition with 64KB-aligned blocks.
# This board's Spansion/Infineon flash (mfr 0x01) ignores 4KB sector erase
# outside its parameter sectors, so esptool's tail-of-image erase silently
# fails and verification reports "MD5 of file does not match data in flash".
# Only app0 is erased; NVS (vault data) is left untouched.
import os
Import("env")

APP_OFFSET = "0x10000"
APP_SIZE = "0x200000"  # factory app size in partitions.csv


def erase_app(source, target, env):
    esptool = os.path.join(env.PioPlatform().get_package_dir("tool-esptool"), "esptool")
    port = env.subst("$UPLOAD_PORT")
    cmd = f'"{esptool}" --port {port} erase-region {APP_OFFSET} {APP_SIZE}'
    print("Erasing app partition:", cmd)
    if env.Execute(cmd):
        env.Exit(1)


env.AddPreAction("upload", erase_app)
