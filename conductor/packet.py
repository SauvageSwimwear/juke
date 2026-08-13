MSG_NOTE_ON  = 0x01
MSG_NOTE_OFF = 0x02
MSG_PROGRAM  = 0x03
MSG_CC       = 0x04
MSG_CONFIG   = 0x05

_VALID = {MSG_NOTE_ON, MSG_NOTE_OFF, MSG_PROGRAM, MSG_CC, MSG_CONFIG}


def encode(msg_type, channel, data1, data2) -> bytes:
    if msg_type not in _VALID:
        raise ValueError(f"unknown msg_type 0x{msg_type:02X}")
    buf = bytes([msg_type, channel & 0x0F, data1 & 0x7F, data2 & 0x7F, 0x00])
    return buf + bytes([buf[0] ^ buf[1] ^ buf[2] ^ buf[3] ^ buf[4]])
