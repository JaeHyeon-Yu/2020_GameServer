myid = 9999
dir	= 0
cnt = 0

function SetUID(x)
	myid = x
end

function Event_Player_Move(p_id)
	if API_Get_x(p_id) == API_Get_x(myid) then
		if API_Get_y(p_id) == API_Get_y(myid) then
			API_SendMessage(myid, p_id, "Hello")
			dir = math.random(0,3)
			cnt = 0
			return true
		end
	end
	return false
end

function Event_Escape_Move()
	x = API_Get_x(myid)
	y = API_Get_y(myid)

	if dir == 0 then	
		x = x + 1
	elseif dir == 1 then	
		x = x - 1
	elseif dir == 2 then	
		y = y + 1
	else	
		y = y - 1
	end


	cnt = cnt + 1
	-- ¿Ãµø≥°
	if cnt == 3 then
		API_SendMessage(myid, player, "Bye")
		cnt = 0
		dir = 0
	end
	return x, y, cnt
end
