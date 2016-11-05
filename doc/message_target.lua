-- message_target
-- @short: send a utf-8 coded text message to a frameserver
-- @inargs: tgtvid, msg
-- @outargs: bytes_left
-- @longdescr: In some rare occasions (custom applications and clipboard
-- not using bchunk- method of transfer) it makes sense to send short custom
-- text messages to a target frameserver. This function takes a string,
-- valides it as utf-8 and breaks it down to event- sized chunks and adds
-- to the target frameserver incoming eventqueue. The function returns 0 if
-- the entire message was sent, a negative value if the string failed to
-- validate or a positive number indicating the number of bytes left. The
-- latter case only occurs if the target event queue is full.
-- @group: targetcontrol
-- @cfunction: targetmessage
-- @related: target_input
function main()
#ifdef MAIN
	target_alloc("test", function(source, status)
		if (status.kind == "connected") then
			message_target(source, "welcome");
			return shutdown();
		end
	end);
#endif
end
