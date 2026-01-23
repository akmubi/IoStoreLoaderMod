function Register()
	return "48 89 5C 24 10 48 89 6C 24 18 56 57 41 54 41 56 41 57 48 83 EC 40 45 33 E4 48 8B F1 48 8B 0D ?? ?? ?? ?? 41 8B DC 89 5C 24 70 4C 8B F2 48 85 C9 75 0C E8 ?? ?? ?? ?? 48 8B 0D"
end

function OnMatchFound(MatchAddress)
	local MovInstrSize = 0x07
	local MovInstr     = MatchAddress + 0x1C
	local Offset       = DerefToInt32(MovInstr + 0x03)
	local Destination  = MovInstr + MovInstrSize + Offset

	return Destination
end
