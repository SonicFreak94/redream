test_fipr:
  # REGISTER_IN fr4  0x3f800000
  # REGISTER_IN fr5  0xc0000000
  # REGISTER_IN fr6  0xc0400000
  # REGISTER_IN fr7  0x40800000
  # REGISTER_IN fr12 0x40800000
  # REGISTER_IN fr13 0xc0400000
  # REGISTER_IN fr14 0xc0000000
  # REGISTER_IN fr15 0x3f800000
  fipr fv4, fv12
  rts 
  nop
  # REGISTER_OUT fr15 0x41a00000
