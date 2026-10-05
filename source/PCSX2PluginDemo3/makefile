.PHONY: all clean
all:
	powershell -NoProfile -ExecutionPolicy Bypass -File "../../tools/build-module.ps1" -Project module.json
clean:
	powershell -NoProfile -ExecutionPolicy Bypass -File "../../tools/build-module.ps1" -Project module.json -Clean
