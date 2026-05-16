#!/usr/bin/env python3
"""
MTK Bridge — JSON-RPC over stdin/stdout for PhoneToolbox.

Protocol:
  -> {"id":1,"method":"connect","params":{}}
  <- {"id":1,"result":{"status":"ok"}}

  -> {"id":2,"method":"seccfg","params":{"lock":false}}
  <- {"id":2,"result":{"status":"ok"}}

  -> {"id":3,"method":"printgpt","params":{}}
  <- {"id":3,"result":{"status":"ok","partitions":[{"name":"boot","offset":0,"length":65536},...]}}

Errors:
  <- {"id":1,"error":{"code":-1,"message":"something failed"}}
"""

import json
import logging
import os
import sys
import traceback
from binascii import hexlify

# Add mtkclient to path (relative to this script / mtkclient dir)
_script_dir = os.path.dirname(os.path.abspath(__file__))
_mtkclient_dir = os.path.join(_script_dir, "..", "mtkclient")
if os.path.isdir(_mtkclient_dir):
    sys.path.insert(0, _mtkclient_dir)

from mtkclient.config.mtk_config import MtkConfig
from mtkclient.Library.mtk_class import Mtk
from mtkclient.Library.DA.mtk_da_handler import DaHandler
from mtkclient.Library.gui_utils import logsetup


class MtkBridge:
    def __init__(self):
        self.mtk = None
        self.da_handler = None
        self.config = None
        self.connected = False
        self._setup_logging()

    def _setup_logging(self):
        logging.basicConfig(
            level=logging.INFO,
            format="%(message)s",
            stream=sys.stderr
        )
        self.log = logging.getLogger("mtk_bridge")

    def _send_response(self, req_id, result=None, error=None):
        """Send a JSON response to stdout."""
        resp = {"id": req_id}
        if error is not None:
            resp["error"] = error
        else:
            resp["result"] = result
        sys.stdout.write(json.dumps(resp, ensure_ascii=False) + "\n")
        sys.stdout.flush()

    def _send_progress(self, percent, message=""):
        """Send an unsolicited progress update."""
        resp = {"method": "__progress", "params": {"percent": percent, "message": message}}
        sys.stdout.write(json.dumps(resp, ensure_ascii=False) + "\n")
        sys.stdout.flush()

    def handle_connect(self, req_id, params):
        """Initialize and connect to MTK device in BROM/preloader mode."""
        if self.connected:
            self._send_response(req_id, {"status": "already_connected"})
            return

        try:
            self._send_progress(5, "Initializing mtkclient...")

            loglevel = logging.INFO
            self.config = MtkConfig(loglevel=loglevel, gui=None, guiprogress=None)

            serialport = params.get("serialport", None)
            if serialport == "":
                serialport = None

            # Optional overrides from params
            if params.get("loader"):
                self.config.loader = params["loader"]
            if params.get("ptype"):
                self.config.ptype = params["ptype"]

            self.mtk = Mtk(config=self.config, loglevel=loglevel, serialportname=serialport)

            self._send_progress(20, "Handshaking with preloader...")

            # Initialize preloader connection (handshake + HW code detection)
            if not self.mtk.preloader.init():
                self._send_response(req_id, error={
                    "code": -2,
                    "message": "Preloader init failed. Device not in BROM/preloader mode?"
                })
                self.mtk.port.close()
                self.mtk = None
                return

            self._send_progress(50, "Loading DA (Download Agent)...")

            # Connect DA handler and load DA
            self.da_handler = DaHandler(self.mtk, loglevel)
            directory = params.get("directory", None)
            mtk = self.da_handler.connect(self.mtk, directory)

            if mtk is None:
                self._send_response(req_id, error={
                    "code": -3,
                    "message": "DA connection failed"
                })
                self.mtk.port.close()
                self.mtk = None
                return

            self._send_progress(75, "Configuring DA...")
            self.mtk = mtk
            mtk = self.da_handler.configure_da(self.mtk)

            if mtk is None:
                self._send_response(req_id, error={
                    "code": -4,
                    "message": "DA configuration failed"
                })
                self.mtk.port.close()
                self.mtk = None
                return

            self.mtk = mtk
            self.connected = True
            self._send_progress(100, "Connected")
            self._send_response(req_id, {
                "status": "ok",
                "chip": self.mtk.config.chipconfig.name if self.mtk.config.chipconfig else "unknown",
                "hwcode": self.mtk.config.hwcode
            })

        except Exception as e:
            self._send_response(req_id, error={
                "code": -1,
                "message": str(e),
                "traceback": traceback.format_exc()
            })

    def handle_disconnect(self, req_id, params):
        """Disconnect from MTK device."""
        if self.mtk and self.mtk.port:
            try:
                self.mtk.port.close()
            except Exception:
                pass
        self.connected = False
        self.mtk = None
        self.da_handler = None
        self._send_response(req_id, {"status": "ok"})

    def handle_seccfg(self, req_id, params):
        """Set seccfg lock state."""
        if not self._ensure_connected(req_id):
            return

        lock = params.get("lock", True)
        try:
            self._send_progress(10, "Reading seccfg partition...")
            result = self.mtk.daloader.seccfg(lock)
            if result[0]:
                self._send_progress(100, "Done")
                self._send_response(req_id, {"status": "ok", "message": result[1]})
            else:
                self._send_response(req_id, error={
                    "code": -10,
                    "message": result[1]
                })
        except Exception as e:
            self._send_response(req_id, error={
                "code": -1,
                "message": str(e),
                "traceback": traceback.format_exc()
            })

    def handle_printgpt(self, req_id, params):
        """Print GPT partition table and return partition list."""
        if not self._ensure_connected(req_id):
            return

        try:
            data, guid_gpt = self.mtk.daloader.get_gpt()
            if not guid_gpt:
                self._send_response(req_id, error={
                    "code": -11,
                    "message": "Failed to read GPT"
                })
                return

            partitions = []
            for entry in guid_gpt.partentries:
                partitions.append({
                    "name": entry.name,
                    "offset": entry.start_lba * guid_gpt.sector_size,
                    "length": entry.sector_count * guid_gpt.sector_size,
                    "start_lba": entry.start_lba,
                    "sector_count": entry.sector_count,
                    "guid": entry.guid,
                    "attributes": entry.attributes
                })

            self._send_response(req_id, {
                "status": "ok",
                "sector_size": guid_gpt.sector_size,
                "partitions": partitions
            })

        except Exception as e:
            self._send_response(req_id, error={
                "code": -1,
                "message": str(e),
                "traceback": traceback.format_exc()
            })

    def handle_read_partition(self, req_id, params):
        """Read a partition to a file. Uses da_handler file-based read."""
        if not self._ensure_connected(req_id):
            return

        partition = params.get("partition", "")
        output_path = params.get("output_path", "")

        if not partition or not output_path:
            self._send_response(req_id, error={
                "code": -20,
                "message": "Missing 'partition' or 'output_path' parameter"
            })
            return

        try:
            self._send_progress(5, f"Reading partition {partition}...")

            # Use da_handler's file-based read
            self.da_handler.da_read(
                partitionname=partition,
                parttype="user",
                filename=output_path
            )

            if os.path.exists(output_path) and os.path.getsize(output_path) > 0:
                self._send_progress(100, "Read complete")
                self._send_response(req_id, {"status": "ok", "path": output_path})
            else:
                self._send_response(req_id, error={
                    "code": -12,
                    "message": f"Failed to read partition {partition}"
                })

        except Exception as e:
            self._send_response(req_id, error={
                "code": -1,
                "message": str(e),
                "traceback": traceback.format_exc()
            })

    def handle_write_partition(self, req_id, params):
        """Write a file to a partition. Uses da_handler file-based write."""
        if not self._ensure_connected(req_id):
            return

        partition = params.get("partition", "")
        image_path = params.get("image_path", "")

        if not partition or not image_path:
            self._send_response(req_id, error={
                "code": -20,
                "message": "Missing 'partition' or 'image_path' parameter"
            })
            return

        if not os.path.exists(image_path):
            self._send_response(req_id, error={
                "code": -21,
                "message": f"File not found: {image_path}"
            })
            return

        try:
            self._send_progress(10, f"Writing {image_path} to {partition}...")

            # Use da_handler's file-based write
            self.da_handler.da_write(
                parttype="user",
                filenames=[image_path],
                partitions=[partition]
            )

            self._send_progress(100, "Write complete")
            self._send_response(req_id, {"status": "ok"})

        except Exception as e:
            self._send_response(req_id, error={
                "code": -1,
                "message": str(e),
                "traceback": traceback.format_exc()
            })

    def handle_reset(self, req_id, params):
        """Reset/shutdown the device."""
        if not self._ensure_connected(req_id):
            return

        try:
            bootmode = params.get("bootmode", 0)
            self.mtk.daloader.shutdown(bootmode=bootmode)
            self.connected = False
            self._send_response(req_id, {"status": "ok", "message": "Reset command sent"})
        except Exception as e:
            self._send_response(req_id, error={
                "code": -1,
                "message": str(e),
                "traceback": traceback.format_exc()
            })

    def _ensure_connected(self, req_id):
        """Check if MTK device is connected."""
        if not self.connected or self.mtk is None:
            self._send_response(req_id, error={
                "code": -30,
                "message": "Not connected. Call 'connect' first."
            })
            return False
        return True

    def dispatch(self, request):
        """Parse and dispatch a JSON-RPC request."""
        req_id = request.get("id", 0)
        method = request.get("method", "")
        params = request.get("params", {})

        handlers = {
            "connect": self.handle_connect,
            "disconnect": self.handle_disconnect,
            "seccfg": self.handle_seccfg,
            "printgpt": self.handle_printgpt,
            "read_partition": self.handle_read_partition,
            "write_partition": self.handle_write_partition,
            "reset": self.handle_reset,
        }

        handler = handlers.get(method)
        if handler is None:
            self._send_response(req_id, error={
                "code": -99,
                "message": f"Unknown method: {method}"
            })
            return

        handler(req_id, params)

    def run(self):
        """Main loop: read JSON lines from stdin, dispatch, respond."""
        # Signal readiness
        sys.stderr.write("mtk_bridge: ready\n")
        sys.stderr.flush()

        for line in sys.stdin:
            line = line.strip()
            if not line:
                continue

            try:
                request = json.loads(line)
                self.dispatch(request)
            except json.JSONDecodeError as e:
                self._send_response(
                    request.get("id", 0) if isinstance(request, dict) else 0,
                    error={"code": -100, "message": f"Invalid JSON: {str(e)}"}
                )
            except Exception as e:
                self._send_response(
                    request.get("id", 0) if isinstance(request, dict) else 0,
                    error={"code": -1, "message": str(e), "traceback": traceback.format_exc()}
                )


if __name__ == "__main__":
    bridge = MtkBridge()
    bridge.run()
