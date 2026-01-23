function Register()
	return "45 84 C0 48 C7 41 10 00 00 00 00 B8 FF FF FF FF 4C 8D 1D"
end

function OnMatchFound(MatchAddress)
	local JmpInstrSize = 0x07
	local JmpInstr     = MatchAddress + 0x10
	local Offset       = DerefToInt32(JmpInstr + 0x03)
	local Destination  = JmpInstr + JmpInstrSize + Offset

	return Destination
end
