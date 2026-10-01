"""Wire contract for the vendor buttons/analogs modules, without a live game."""

from contextlib import redirect_stdout
import importlib.util
import io
import json
from pathlib import Path
import unittest
from unittest.mock import patch


SPEC = importlib.util.spec_from_file_location(
    "game_api", Path(__file__).resolve().parents[2] / "tools" / "game_api.py")
game_api = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(game_api)


class ProtocolSocket:
    def __init__(self):
        self.requests = []
        self.reply = b""
        self.closed = False

    def setsockopt(self, *_args):
        pass

    def close(self):
        self.closed = True

    def sendall(self, wire):
        assert wire.endswith(b"\0") and b"\0" not in wire[:-1]
        request = json.loads(wire[:-1])
        assert isinstance(request["id"], int) and request["id"] >= 0
        assert isinstance(request["params"], list)
        self.requests.append(request)
        if request["function"] == "read":
            names = ["Start", "Scope Right", "Gun Pressed"] if request["module"] == "buttons" else ["Gun X", "Gun Y"]
            data = [[name, 0.5, False] for name in names]
        else:
            data = []
        self.reply = json.dumps({"id": request["id"], "errors": [], "data": data}).encode() + b"\0"

    def recv(self, _maximum):
        # A NUL delimiter may arrive in a different packet than the JSON body.
        chunk, self.reply = self.reply[:1], self.reply[1:]
        return chunk


class ProtocolTests(unittest.TestCase):
    def run_client(self, args):
        fake = ProtocolSocket()
        with patch.object(game_api.socket, "create_connection", return_value=fake) as connect, \
                patch.object(game_api.time, "sleep"), redirect_stdout(io.StringIO()):
            result = game_api.main(args)
        self.assertEqual(result, 0)
        connect.assert_called_once_with(("127.0.0.1", 5730), 3.0)
        self.assertTrue(fake.closed)
        return fake.requests

    def test_first_plaintext_request_is_read_without_handshake(self):
        requests = self.run_client(["read", "buttons"])
        self.assertEqual(requests, [{"id": 1, "module": "buttons", "function": "read", "params": []}])

    def test_button_boolean_and_targeted_nested_reset(self):
        requests = self.run_client(["press", "Start"])
        self.assertEqual([r["function"] for r in requests], ["read", "write", "write_reset"])
        self.assertEqual(requests[1]["params"], [["Start", True]])
        # bool is an accepted vendor button type; it must not become a string.
        self.assertIs(type(requests[1]["params"][0][1]), bool)
        self.assertEqual(requests[2]["params"], [["Start"]])

    def test_analog_value_is_numeric_and_both_axes_release(self):
        requests = self.run_client(["aim", "0", "1", "--scope", "--fire"])
        analog_write = next(r for r in requests if r["module"] == "analogs" and r["function"] == "write")
        self.assertEqual(analog_write["params"], [["Gun X", 0.0], ["Gun Y", 1.0]])
        self.assertTrue(all(type(row[1]) is float for row in analog_write["params"]))
        resets = {r["module"]: r["params"] for r in requests if r["function"] == "write_reset"}
        self.assertEqual(resets["analogs"], [["Gun X"], ["Gun Y"]])
        self.assertEqual(resets["buttons"], [["Scope Right"], ["Gun Pressed"]])

    def test_reset_all_has_no_named_parameters(self):
        requests = self.run_client(["reset", "--all"])
        self.assertEqual([(r["module"], r["function"], r["params"]) for r in requests],
                         [("buttons", "write_reset", []), ("analogs", "write_reset", [])])


if __name__ == "__main__":
    unittest.main()
