#!/usr/bin/python
#Console Loader for TinyG
#This version of the Console loader is meant to be used with 308.11 + versions of TinyG
#Riley Porter
#Synthetos.com

import serial
import sys
import random
from optparse import OptionParser

logo = """
  _______ _              _____ 
 |__   __(_)            / ____|
    | |   _ _ __  _   _| |  __ 
    | |  | | '_ \| | | | | |_ |
    | |  | | | | | |_| | |__| |
    |_|  |_|_| |_|\__, |\_____|
    CONSOLE        __/ |       
                  |___/        """

def usage():
    print """[?] Usage: 
         -f indicates the filename to be sent to TinyG.
         -s indicates the serial port that TinyG is connected to.
EXAMPLE:
           python TinyConsole.py -f gcodefile.nc -s COM7
           or
           python TinyConsole.py -filename gcodefile.nc -serialport /dev/tty/USB0"""

    

def main():
    #Option Parser Code
    print logo
    parser = OptionParser()
    parser.add_option("-f", "--file",
                      dest="filename",
                      help="Send this file to TinyG's serial port",
                      metavar="FILE")
    
    parser.add_option("-s",
                      "--serialport",
                      dest="serial_port",
                      help="The serial port Tinyg is using.")
    
    (options, args) =  parser.parse_args()
    if options.filename == None or options.serial_port == None:
        print "[!] Error: Incorrect Usage...."
        usage()
        sys.exit()
        
    #End Option Parser Code
    
    try:
        s = serial.Serial(options.serial_port, 115200, timeout=.5) #Setup Serial port COnnection
        s.xonxoff = True #Turn on software flow control.  TinyG supports this
        
    except:
        print "[!] Error Opening Serial Port"
        print "\t-Make sure you specified the correct serial port and TinyG is powered."
        sys.exit()
     
    #Try to open the gcode file that was passed via the command line
    try:
        f = open(options.filename, "r")
        print "[*] Opening %s Gcode File" % options.filename
    except IOError:
        print("Error Opening %s, make sure it exists?" % options.filename)
        print("Exiting....")
        sys.exit()
    
    #Begin Processing the Gcode File
    print "[*] Begin Processing Gcode File"    
    for line in f.readlines():
        s.writelines(line)
        y = s.readline()
        print y.rstrip()
        #if y != "":
            #if "ok" in y:
                #print "\t[#]LINE: %s" % line.rstrip()
            #else:
                #print "\t[#]LINE: %s" % line.rstrip()
                
    print "[*]Job Complete"
    sys.exit()

if __name__ == "__main__":
    main()

    
