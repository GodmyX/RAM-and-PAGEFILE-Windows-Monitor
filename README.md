PAGEFILE/SWAP use & RAM use MONITOR
===============================================
Windows 7, Windows 10, Windows 11 are supported (may run on Vista too)
===============================================


It loads really fast. It shows you both how much RAM you use, but also how much of the PAGEFILE (the SWAP) you're using, thus giving you some idea whether your pagefile should or should not be increased.
----------------------------

The Windows Task Manager (and System Resources Monitor) give this information only for the Physical RAM, but it is difficult to deduce the PageFile/swap use from the rest of the information Windows natively gives to the user. This Monitor shows it to you plain & simple.

-----------------------------
====FILL THE RAM UP TEST====

RIGHT click on the area of the Window to run a "RAM fill test" where gradually more and more memory is allocated into your RAM (not real data/writes, only mere allocations) until it fills up and then you can watch how the "fake data" are dumped into the pagefile. Before the pagefile fills 100%, the program stops and in 5 seconds releases the memory back. (Or right click and press "STOP").

-----------------------------
=== STAY ON TOP =====

Right click and tick whether you want the program to stay on top or not. Also, the next time you open the program, it should should find itself there, where you put it once as you were closing it.

==============================
==============================

Generally speaking, having a large swap pagefile (even much larger than your physical memory in case you've got less than 32 GB), with an SSD makes total wonders to how fast the system will be (not so on HDD, but with an SSD - yes). For those who are worried about NAND overwrites when it comes to SSD & the ultimate WRITES limit: if you've got a modern SSD with many TBW (300 - 600) then even with extensive writes every day in the form of a LARGE PAGEFILE, you are likely to not be able to "consume up the TBW" of the SSD sooner than in 15-30 years, realistically. 

And a SSD large swap file can do wonders even to an older computer with 8 GB of RAM running Windows 7. For running things like Firefox even with 30-50 tabs opened, an 8 GB RAM laptop with a 24 GB SSD-based pagefile will behave like a native 32 GB RAM computer (in these cases).

==============================
==============================

You can use the premade binaries in the folder "binaries". The .exe is standalone, it only needs the settings.ini file (if not present, it creates it itself - to store your preferences).


=== HOW TO COMPILE THE APP ===

1) Install the newest MiniGW-64. https://www.msys2.org/

2) make sure that the installation path like c:\msys64\mingw64\bin\ or c:\MinGW\bin\ is set in your ENVIRONMENT VARIABLES, specifically in the System (or User) variable PATH

3) run "01 compile resources.bat" <- so you will get the icon
	OR run manually in a command line this:

		windres resources.rc -o resources.o

5) run "02 compile the APP.bat"
	
	OR run manually in the command line this:
		gcc -I"\include" resources.o -o "RAM Monitor.exe" -mwindows -lgdi32 -lcomctl32 -lole32 -loleaut32 -luuid -lpsapi -static-libgcc
		
6) enjoy the APP
