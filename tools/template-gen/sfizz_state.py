# Estado VST3 do sfizz (SfizzVstState versao 0: ficheiro .sfz, volume, vozes, oversampling, preload) embrulhado como o
# JUCE o guarda (copyXmlToBinary de <VST3PluginState><IComponent>base64 JUCE</IComponent>) e em base64 normal para o
# campo "state" do Core (engine juce_host.cpp: Base64::convertFromBase64 -> setStateInformation).
import base64, struct, sys
TABLE = ".ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+"
def juce_b64(data: bytes) -> str:
    n = len(data); bits = int.from_bytes(data, "little"); chars = ((n << 3) + 5) // 6
    return str(n) + "." + "".join(TABLE[(bits >> (i * 6)) & 63] for i in range(chars))
def juce_b64_decode(s: str) -> bytes:
    n, rest = s.split(".", 1); bits = 0
    for i, c in enumerate(rest): bits |= TABLE.index(c) << (i * 6)
    return (bits & ((1 << (8 * int(n))) - 1)).to_bytes(int(n), "little")
def str8(s: str) -> bytes:
    b = s.encode("utf-8") + b"\0"; return struct.pack("<i", len(b)) + b
def sfizz_component(sfz: str) -> bytes:
    return struct.pack("<Q", 0) + str8(sfz.replace("\\", "/")) + struct.pack("<fiii", 0.0, 64, 0, 8192)
def core_state(sfz: str) -> str:
    comp = sfizz_component(sfz)
    xml = '<?xml version="1.0" encoding="UTF-8"?> <VST3PluginState><IComponent>' + juce_b64(comp) + "</IComponent></VST3PluginState>"
    body = xml.encode("utf-8") + b"\0"
    blob = struct.pack("<II", 0x21324356, len(body) - 1) + body
    assert juce_b64_decode(juce_b64(comp)) == comp
    return base64.b64encode(blob).decode()
if __name__ == "__main__":
    print(core_state(sys.argv[1]))
