# eeg_view_python

This MEF 3.0 viewer is a python-based GUI with a c-code server.  Both the python and c are intended to be platform independent, and have been tested on Windows 10 and MacOS X 11.6.

## Page Server
The page server code is in the page_server subdirectory.  It requires the code from the [meflib repositiory](https://github.com/msel-source/meflib).  The output executable should be either "eeg_page_server" (for Mac) or "eeg_page_server.exe" (for Windows) and should be placed at the same directory level as the python GUI code.

## Python GUI
To launch, use "python3 eeg_view.py".  It creates a temporary folder in the appropriate location, and uses files within that temporary folder to communicate with the page server.  These temp folders and temp files will be automatically deleted (depending on the OS, it could be upon reboot, or after 3 days, etc), so neither the GUI or server attempts to delete the files.

## Sample Data
Sample data for MEF 3.0 data can be found [here](https://github.com/msel-source/sampledata).  Below is that sample data plotted using this viewer, on the MacOS operating system.

![plot screenshot](https://github.com/msel-source/sampledata/blob/master/eeg_view_plot3.jpg?raw=true)
