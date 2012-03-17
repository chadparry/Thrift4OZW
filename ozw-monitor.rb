=begin
Thrift4OZW - An Apache Thrift wrapper for OpenZWave
----------------------------------------------------
Copyright (c) 2011 Elias Karakoulakis <elias.karakoulakis@gmail.com>

SOFTWARE NOTICE AND LICENSE

Thrift4OZW is free software: you can redistribute it and/or modify
it under the terms of the GNU Lesser General Public License as published
by the Free Software Foundation, either version 3 of the License,
or (at your option) any later version.

Thrift4OZW is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public License
along with Thrift4OZW.  If not, see <http://www.gnu.org/licenses/>.

for more information on the LGPL, see:
http://en.wikipedia.org/wiki/GNU_Lesser_General_Public_License
=end

# --------------------------
#
# monitor.rb: a rudimentary OpenZWave notification monitor
# spits out all OpenZWave activity posted by Main.cpp to STOMP server
#
# ---------------------------

require 'rubygems'
require 'onstomp'
require 'bit-struct'

require 'ozw-headers'
require 'zwave-command-classes'

#~ // ID Packing:
#~ // Bits
#~ // 24-31:	8 bits. Node ID of device
#~ // 22-23:	2 bits. genre of value (see ValueGenre enum).
#~ // 14-21:	8 bits. ID of command class that created and manages this value.
#~ // 12-13:	2 bits. Unused.
#~ // 04-11:	8 bits. Index of value within all the value created by the command class
#~ //                  instance (in configuration parameters, this is also the parameter ID).
#~ // 00-03:	4 bits. Type of value (bool, byte, string etc).
class OZW_EventID_id < BitStruct
    unsigned    :node_id,       8, "Node ID of device"
    unsigned    :value_genre,   2, "Value Genre"
    unsigned    :cmd_class,     8, "command class"
    unsigned    :unused1,       2, "(unused)"
    unsigned    :value_idx,     8, "value index"
    unsigned    :value_type,    4, "value type( bool, byte, string etc)"
end

#~ // ID1 Packing:
#~ // Bits
#~ // 24-31	8 bits. Instance Index of the command class.
class OZW_EventID_id1 < BitStruct
    unsigned    :cmd_class_idx, 8, "cmd class index"
    unsigned    :unused2   , 24, "(unused)"    
end

threads = []
currdir = Dir.getwd.split(File::SEPARATOR)
cpp_src = File.join(currdir[0..-2], "open-zwave-read-only", "cpp", "src")
unless Dir.exists?(cpp_src)
  raise "OpenZWave source directory not found!"
end

notificationtypes, valuegenres, valuetypes = parse_ozw_headers(cpp_src) # in ozw-headers.rb
 
#~ threads <<  Thread.new() {
    begin
        server = OnStomp.connect "stomp://localhost"
        server.subscribe '/queue/zwave/monitor' do |msg|
            # Invoked every time the broker delivers a MESSAGE frame for the
            # SUBSCRIBE frame generated by this method call.
            puts "\n------ ZWAVE MESSAGE (#{Time.now}) ------"
            msg.headers.each { |hdr, val|
                #puts "header: #{hdr} == #{val}"
                i = 0
                case hdr
                when "ValueID" then
                    id = [val.delete(' ')[-8..-1].to_i(16)].pack("N")
                    id1 = [val.delete(' ')[0..-9].to_i(16)].pack("N")
                    #
                    b = OZW_EventID_id.new(id)
                    puts b.inspect
                    puts "  node ID of device: #{b.node_id}"
                    puts "        value genre: #{valuegenres[b.value_genre.to_i].join(': ')}"
                    puts "         value type: #{valuetypes[b.value_type.to_i].join(': ')}"
                    puts "          value idx: #{b.value_idx}"
                    puts "      command class: #{b.cmd_class} (#{CommandClassesByID[b.cmd_class]})"
                    b = OZW_EventID_id1.new(id1)
                    puts "     subcommand idx: #{b.cmd_class_idx}"
                when "NotificationType" then
                    puts "  notification type: #{notificationtypes[val.to_i(16)].join(': ')}"
                else
                    puts "   #{hdr} : #{val}"
                end
            }
        end
    rescue Exception => e
        puts e.inspect
        puts e.backtrace.join("\n  ")
    #~ ensure
        #~ server.disconnect
    end
#~ }

#~ threads.each { |aThread|  aThread.join }