-- MoonGen script to generate load consisting of line rate
-- bursts.
-- Author: Amaury Van Bemten <amaury.van-bemten@tum.de>

local mg     = require "moongen"
local memory = require "memory"
local device = require "device"
local filter = require "filter"
local stats  = require "stats"
local log    = require "log"
local os     = require "os"

require "utils"

local DST_MAC="02:9e:66:a6:d0:01"
local SRC_MAC="02:60:02:e1:00:01"
local SRC_IP_BASE	= "10.0.0.1" -- actual address will be SRC_IP_BASE + random(0, flows)
local DST_IP		= "20.0.0.1"
local SRC_PORT		= 5000
local DST_PORT		= 6000

function configure(parser)
	parser:description("Generates line rate bursts of traffic.")
	parser:argument("txDev", "Device to transmit from."):convert(tonumber)
	parser:option("-f --flows", "Number of flows (incremented source IP)."):default(1):convert(tonumber)
	parser:option("-s --size", "Packet size."):default(60):convert(tonumber)
	parser:option("-d --duration", "How long in seconds to run the script."):default(10):convert(tonumber)
	parser:option("-b --burst", "Number of packets in a 1 Gbps burst."):default(5000):convert(tonumber)
	parser:option("-w --delay", "How long to wait between bursts (in ms)."):default(150):convert(tonumber)
end

function master(args)
	txDev = device.config{port = args.txDev, rxQueues = 1, txQueues = 1}
	device.waitForLinks()
	log:info("Making sure the device negotiated 1G or 10G")
	while true do
		txSpeed = txDev:getLinkStatus().speed
		if txSpeed == 1000 or txSpeed == 10000 then
			break
		end
	end
	mg.sleepMillis(10000)
	mg.startTask("loadSlave", txDev:getTxQueue(0), args.size, args.flows, args.duration, args.burst, args.delay)
	mg.waitForTasks()
end

local function fillUdpPacket(buf, len)
	buf:getUdpPacket():fill{
		ethSrc = SRC_MAC,
		ethDst = DST_MAC,
		ip4Src = SRC_IP,
		ip4Dst = DST_IP,
		udpSrc = SRC_PORT,
		udpDst = DST_PORT,
		pktLength = len
	}
end

function loadSlave(queue, size, flows, duration, burstSize, delay)
	local mempool = memory.createMemPool(function(buf)
		fillUdpPacket(buf, size)
	end)
	local bufs = mempool:bufArray(63)
	local counter = 0
	local txCtr = stats:newDevTxCounter(queue, "csv")
	local baseIP = parseIPAddress(SRC_IP_BASE)
	local startTime = os.time()
	local finishTime = startTime + duration
	local burstCounter = 0
	while mg.running() and os.time() < finishTime do
		bufs:alloc(size)
		for i, buf in ipairs(bufs) do
			local pkt = buf:getUdpPacket()
			pkt.ip4.src:set(baseIP + counter)
			counter = incAndWrap(counter, flows)
		end
		-- UDP checksums are optional, so using just IPv4 checksums would be sufficient here
		bufs:offloadUdpChecksums()
		queue:send(bufs)
		txCtr:update()
		burstCounter = burstCounter + 1
		if burstCounter == burstSize then
			burstCounter = 0
			mg.sleepMillis(delay)
		end
	end
	txCtr:finalize()
end
