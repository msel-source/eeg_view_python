
#    eeg_view - python user interface to view MEF 3.0 data
#    Copyright (C) 2021 Mayo Foundation, Rochester MN. All rights reserved.
#
#    Written by Dan Crepeau
#
#    This program is free software: you can redistribute it and/or modify
#    it under the terms of the GNU General Public License as published by
#    the Free Software Foundation, either version 3 of the License, or
#    (at your option) any later version.
#
#    This program is distributed in the hope that it will be useful,
#    but WITHOUT ANY WARRANTY; without even the implied warranty of
#    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#    GNU General Public License for more details.
#
#    You should have received a copy of the GNU General Public License
#    along with this program.  If not, see <https://www.gnu.org/licenses/>.
#
#    To install:
#
#      pip3 install numpy
#      pip3 install PyQt5
#      pip3 isntall matplotlib
#
#    Tested on Mac OS v11.6 and Windows 10 running Python version 3.9
#
#    Acknowledgements: This code is inspired by eeg_view_DB.m, a Matlab
#    MEF eeg viewer written by members of the Mayo Systems Electrophysiology
#    Lab (MSEL), including Matt Stead, Ben Brinkmann, Vince Vasoli and Mark
#    Bower.  A key difference between this and eeg_view_DB is (besides being
#    in Python), this veiwer is timestamp based, rather than sample number
#    based, so discontinuities in the data are properly rendered.


    
import sys
from PyQt5 import QtCore
from PyQt5.QtCore import Qt, QEvent
from PyQt5.QtWidgets import QApplication, QWidget, QHBoxLayout, QVBoxLayout, QLabel, QMenuBar, QFileDialog, QMainWindow, QCheckBox, QLineEdit, QComboBox, QProgressBar, QFrame, QDialog, QInputDialog
from PyQt5.QtGui import QPainter
import matplotlib.pyplot as plt
from matplotlib.backends.backend_qt5agg import FigureCanvasQTAgg as FigureCanvas
import matplotlib.patches as patches
import numpy as np
import uuid
import os
from os import walk
import subprocess
import random
import time
import threading
from threading import Event
from threading import Thread
from datetime import datetime
import tempfile


heartbeat_flag = Event()


class HeartbeatThread(Thread):
    def __init__(self, event, path):
        Thread.__init__(self)
        self.stopped = event
        self.path_dir = path

    def run(self):
        cycle = True
        # wait 1 second between writes.
        # but only wait .5 seconds for signal to terminate thread, to make thread more responsive
        while not self.stopped.wait(.5):
            cycle = not cycle
            if cycle:
                with open(self.path_dir+ "heartbeat_ui", 'w') as the_file:
                    the_file.write(str(time.time()))
                    the_file.close()
            
# Create these subclasses so keyboard inputs are properly handled
class MyComboBox(QComboBox):
    def __init__(self, parent):
        QComboBox.__init__(self, parent)
        self.parent = parent

    def keyPressEvent(self, event):
        #super(MainWindow, self).keyPressEvent(event)
        if event.key() == QtCore.Qt.Key_Up:
            self.parent.keyUp()
        if event.key() == QtCore.Qt.Key_Down:
            self.parent.keyDown()
        if event.key() == QtCore.Qt.Key_Right:
            self.parent.keyRight()
        if event.key() == QtCore.Qt.Key_Left:
            self.parent.keyLeft()
        if event.key() == QtCore.Qt.Key_Space:
            self.parent.keySpace()
            
class MyCheckBox(QCheckBox):
    def __init__(self, parent):
        QCheckBox.__init__(self, parent)
        self.parent = parent

    def keyPressEvent(self, event):
        #super(MainWindow, self).keyPressEvent(event)
        if event.key() == QtCore.Qt.Key_Up:
            self.parent.keyUp()
        if event.key() == QtCore.Qt.Key_Down:
            self.parent.keyDown()
        if event.key() == QtCore.Qt.Key_Right:
            self.parent.keyRight()
        if event.key() == QtCore.Qt.Key_Left:
            self.parent.keyLeft()
        if event.key() == QtCore.Qt.Key_Space:
            self.parent.keySpace()
            
# TODO: This code is a start for calibration, but it needs work.  DPI returned by Python
# does not always seem to be accurate.
class CalibrateMonitorDialog(QDialog):
    def __init__(self, parent=None):
        super().__init__(parent)
        
        self.setWindowTitle("Calibrate Monitor")
        
        self.layout = QVBoxLayout()
        message = QLabel("The line below is 200 pixels.")
        self.layout.addWidget(message)
        
        message2 = QLabel("Enter the number of centimeters of the line into the box:")
        self.layout.addWidget(message2)
        
        self.textbox = QLineEdit()
        self.textbox.setMinimumWidth(300)
        self.textbox.setMaximumWidth(300)
        self.textbox.textChanged.connect(self.processInput)
        self.layout.addWidget(self.textbox)
        
        self.message3 = QLabel("Using 200 as dpi value.")
        self.layout.addWidget(self.message3)
        
        self.setLayout(self.layout)
        
        
    def paintEvent(self, event):
        painter = QPainter(self)
        painter.drawLine(20,40,220,40)
        
    def processInput(self):
        f = self.textbox.text()
        try:
            f = float(f)
        except:
            self.message3.setText("Unable to process input from box")
            return
        inches = f / 2.54
        dpi = 200 / inches
        self.message3.setText("Using " + str(dpi) + " as dpi value.")
        

class MainWindow(QMainWindow):

    def __init__(self):
    
        global heartbeat_flag
    
        super().__init__()
        self.setWindowTitle("EEG View")
        
        layout = QVBoxLayout()
        layout_upper = QVBoxLayout()
        layout_lower = QHBoxLayout()
        layout_buffer = QVBoxLayout()
        layout_lower_time = QHBoxLayout()
        layout_lower_checkboxes = QVBoxLayout()
        layout_lower_textboxes = QVBoxLayout()
        layout_lower_textboxes_secpage = QHBoxLayout()
        layout_lower_textboxes_uvcm = QHBoxLayout()


        # create menu bar and actions
        #self.menubar = QMenuBar()
        menubar = self.menuBar()
        #layout.addWidget(self.menubar)
        actionFile = menubar.addMenu("File")
        self.action = actionFile.addAction("Load New Session")
        self.action.triggered.connect(self.load_new_channels)
        self.actionAnnotations = actionFile.addAction("Load Annotations")
        self.actionAnnotations.triggered.connect(self.load_annotations)
        actionSettings = menubar.addMenu("Settings")
        self.actionCalibrate = actionSettings.addAction("Calibrate Monitor")
        self.actionCalibrate.triggered.connect(self.calibrate_monitor)

        # assume the server executable is in the same place as the python script
        self.script_server_path = os.path.dirname(os.path.abspath(__file__))
        
        self.figure = plt.figure()
        self.canvas = FigureCanvas(self.figure)
        

        layout_upper.addWidget(self.canvas)
        #layout_upper.addStretch(1)
        layout.addLayout(layout_upper)
        
        #layout.addWidget(self.canvas)
        
        #time_label = QLabel("Time: ")
        #time_label.setFixedHeight(50)
        #layout_lower_time.addWidget(time_label)

        self.curr_time_label = QLabel("Time: ")
        #self.time_label.setFixedWidth(100)
        self.curr_time_label.setFixedHeight(10)
        #layout_lower_time.setFixedHeight(50)
        layout_lower_time.addWidget(self.curr_time_label)
        layout_lower_time.addStretch(1)
        
        layout_lower.addLayout(layout_lower_time)
        
        self.reverse_voltage = MyCheckBox(self)  #Negative is up
        self.reverse_voltage.setText("Negative is up")
        self.reverse_voltage.setChecked(True)
        self.reverse_voltage.stateChanged.connect(self.onClicked_checkbox_redraw)
        layout_lower_checkboxes.addWidget(self.reverse_voltage)
        
        self.multicolor = MyCheckBox(self)  #Multicolor mode
        self.multicolor.setText("Multicolor mode")
        self.multicolor.setChecked(False)
        self.multicolor.stateChanged.connect(self.onClicked_checkbox_redraw)
        layout_lower_checkboxes.addWidget(self.multicolor)
        
        self.hide_annotations = MyCheckBox(self) #Hide Annotations
        self.hide_annotations.setText("Hide Annotations")
        self.hide_annotations.setChecked(False)
        self.hide_annotations.stateChanged.connect(self.onClicked_checkbox_redraw)
        layout_lower_checkboxes.addWidget(self.hide_annotations)
        
        
        layout_lower.addLayout(layout_lower_checkboxes)
        
        
        self.secpage_label = QLabel("s/page:")
        #self.secpage_label.setFixedHeight(50)
        #self.secpage_box = QLineEdit()
        #self.secpage_box.setFixedHeight(20)
        #self.secpage_box.setFixedWidth(50)
        #self.secpage_box.setText()
        #self.secpage_box.setStyleSheet("border: 1px solid black;")
        #self.secpage_box.setValidator(QtGui.QIntValidator(1, 60))
        #self.secpage_box.setReadOnly(True)
        #self.secpage_box.clearFocus()
        
        self.secpage_combo = MyComboBox(self)
        self.secpage_combo.addItems(["5", "10", "15", "30", "45", "60"])
        combo_index = self.secpage_combo.findText("30")
        self.secpage_combo.setCurrentIndex(combo_index);
        self.secpage_combo.activated.connect(self.onClicked_resend_and_redraw)

        layout_lower_textboxes_secpage.addStretch(1)
        layout_lower_textboxes_secpage.addWidget(self.secpage_label)
        layout_lower_textboxes_secpage.addWidget(self.secpage_combo)
        
        layout_lower_textboxes.addLayout(layout_lower_textboxes_secpage)
        
        self.uvcm_label = QLabel("\u03BCV/cm: " + "".ljust(8))
        self.uvcm_label.setFixedHeight(15)
        self.uvcm_box = QComboBox()
        self.uvcm_box.setVisible(False)
        #self.uvcm_box.setFrameShape(QFrame.Panel)
        
        layout_lower_textboxes_uvcm.addStretch(1)
        layout_lower_textboxes_uvcm.addWidget(self.uvcm_label)
        layout_lower_textboxes_uvcm.addWidget(self.uvcm_box)
        
        
        layout_lower_textboxes.addLayout(layout_lower_textboxes_uvcm)
        layout_lower.addLayout(layout_lower_textboxes)
        
        #layout_upper.setSizeConstraint(QLayout.SetFixedSize)
        
        layout.addLayout(layout_lower)
        
        #buffer_background = patches.Rectangle((50, 100), 40, 30, linewidth=1, edgecolor='r', facecolor='none')
        self.buffer_bar = plt.figure()
        ax = self.buffer_bar.add_axes([0,0,1,1])
        self.canvas_buffer_bar = FigureCanvas(self.buffer_bar)
        #ax.add_patch(buffer_background)
        ax.patch.set_color((0.8, 0.8, 0.8))  # very light grey
        self.canvas_buffer_bar.setFixedHeight(10)
        layout.addWidget(self.canvas_buffer_bar)
        self.buffer_bar.canvas.mpl_connect('button_press_event', self.onClicked_buffer_bar)
       
        self.repaint()
        
        widget = QWidget()
        widget.setLayout(layout)
        self.setCentralWidget(widget)
        
        #self.chan_label_box.drawFrame(QPainter())
        
        # set basic UI variables - a lot of these will become part of the UI
        #self.setGeometry(10, 10, 1300, 800)
        #self.setLayout(layout)
        pix_dims = self.figure.get_size_inches()
        self.axpix = round(pix_dims[0] * self.figure.dpi)
        self.ypix = round(pix_dims[1] * self.figure.dpi)
        self.ylim = -1
        #self.uV_per_pixel = 0.5
        self.raw_page = None
        self.n_displayed = 0
        self.secs_per_page = 30
        self.curr_sec = 0
        self.session_start_time = None
        self.session_end_time = None
        self.server_temp_path = None
        
        #form of event data:
        #self.events = [{'start':1498485704.619469, 'text':'Eyes Open'}, {'start':1498485727.166344, 'text':'Eyes Closed'}]
        self.events = None
        self.discon = None
        
        self.password = None
        heartbeat_flag = Event()
        self.heartbeat_thread = None


    def calibrate_monitor(self):
    
        dlg = CalibrateMonitorDialog(self)
        dlg.exec_()
        
       
    def read_events_from_server(self):
            
        try:
            fp = open(self.server_temp_path + "events")

            # clear out existing events
            self.events = []
        
            event_counter = 0
            for line in fp:
                line = line.rstrip()
                print (line)
            
                words = line.split(',', 2)
            
                if words[1] == "Note":
                    start_time = int(words[0]) / 1000000
                    self.events.append({'start':start_time, 'text':words[2]})
                if words[1] == "Epoch":
                    start_time = int(words[0]) / 1000000
                    epoch_fields = words[2].split(',')
                    print (epoch_fields)
                    # 0 is duration in umsec, 1 is type, 2 is text
                    displayed_text = epoch_fields[1] + ': ' + epoch_fields[2] +  ' (' + str(int(int(epoch_fields[0]) / 1000000)) + ' sec)'
                    self.events.append({'start':start_time, 'text':displayed_text})
            
            fp.close()
            
            return True
            
        except:
            self.events = None
            return False
            
    def read_discon_from_server(self):
        try:
            fp = open(self.server_temp_path + "discon")
            
            self.discon = []
            discon_counter = 0
            for line in fp:
                line = line.rstrip()
                print (line)
                times = line.split(',')
                
                # convert from usec to sec
                times[0] = int(times[0]) / 1000000
                times[1] = int(times[1]) / 1000000
                
                self.discon.append({'start':times[0], 'end':times[1], 'discon_box':None})
                       
                # discon boxes
                self.discon[discon_counter]['discon_box'] = self.buffer_bar.add_axes([0,0,1,1])
                #self.ax_buff_box.patch.set_color('blue')
                self.discon[discon_counter]['discon_box'].patch.set_color((1, 1, 1))  # white
                #self.discon[discon_counter]['discon_box'].patch.set_x(0)
                #self.discon[discon_counter]['discon_box'].patch.set_width(0)
                
                buffer_progress = (times[0] - self.session_start_time) / (self.session_end_time - self.session_start_time)
                buffer_width = (times[1] - times[0]) / (self.session_end_time - self.session_start_time)
                
                # draw buffer marker as a darker grey box
                self.discon[discon_counter]['discon_box'].patch.set_x(buffer_progress)
                self.discon[discon_counter]['discon_box'].patch.set_width(buffer_width)
                
                print (buffer_progress)
                print (buffer_width)
                
                discon_counter = discon_counter + 1
                
            
            fp.close()
            
            self.canvas_buffer_bar.draw()
                
            
            return True
        except:
            print("Exception!")
            self.discon = None
            return False
            
                

    
    def load_csv_annotations(self, filename):
        
        try:
            fp = open(filename, "r")

            # clear out existing events
            self.events = []
        
            event_counter = 0
            for line in fp:
                line = line.rstrip()
                print (line)
            
                words = line.split(',', 1)
                start_time = int(words[0]) / 1000000
                self.events.append({'start':start_time, 'text':words[1]})
            
            fp.close()
            
            return True
            
        except:
            self.events = None
            return False
    
    
    def load_annotations(self):
    
        filetype = "csv (*.csv)"
        event_file = QFileDialog.getOpenFileName(self, "Select Annotations File", filter=filetype)
        print (event_file[0])
        
        # check for .csv files
        if event_file[0].endswith(('.csv', '.CSV', '.Csv')):
            self.load_csv_annotations(event_file[0])
        
        
    def load_new_channels(self):
              
        global heartbeat_flag
        
        filenames = self.get_files()
            
        self.data_dir = filenames
        
        f = []
        for (dirpath, dirnames, filenames) in walk(self.data_dir):
            f.extend(dirnames)
            break
            
        print (f)
      
        
        #set paths - TBD this will be done via UI file picker
        self.channel_paths = []
        file_counter = 0
        for name in f:
            #TBD check for .timd
            if name.find("accel") >= 0:
                continue
            self.channel_paths.append(self.data_dir + "/" + name)
            file_counter = file_counter + 1
        self.n_displayed = file_counter
        
        
        print ("EEG view for python")
        
        password_needed = True
        
        while password_needed:
        
        
            # generate temporary file path for communication between UI and server
            # this will be a different path every time, and the OS is responsible for
            # deleting the data after it is done.
            # self.server_temp_path = "/tmp/eeg_view_" + str(uuid.uuid4()) + "/"
            
            # uuid1 uses current time and machine ID, etc.  uuid4 is purely random.  Since there are no
            # privacy converns here, uuid1 might be better.
            self.server_temp_path = tempfile.gettempdir() + "/" + "eeg_view_" + str(uuid.uuid1()) + "/"
            os.mkdir(self.server_temp_path)
            print ("Temp directory created:", self.server_temp_path)
        
        
            # location of exectuable
            self.page_server_dir = self.script_server_path
        
            # kill old heartbeat thread, if it exists
            if self.heartbeat_thread is not None:
                heartbeat_flag.set()
                time.sleep(.6)  # thread should wait no longer than .5 seconds to check event
                heartbeat_flag = Event()
                
            # start new heartbeat thread, to write heartbeat file every second.
            self.heartbeat_thread = HeartbeatThread(heartbeat_flag, self.server_temp_path)
            self.heartbeat_thread.start()
        
            # open server process in a non-blocking manner
            if os.name == 'nt':
                if self.password is None:
                    subprocess.Popen([self.page_server_dir + "/" + "eeg_page_server.exe", self.server_temp_path])
                else:
                    subprocess.Popen([self.page_server_dir + "/" + "eeg_page_server.exe", self.server_temp_path, self.password])
            else:
                if self.password is None:
                    subprocess.Popen([self.page_server_dir + "/" + "eeg_page_server", self.server_temp_path])
                else:
                    subprocess.Popen([self.page_server_dir + "/" + "eeg_page_server", self.server_temp_path, self.password])
        
            # write initial time and page specs, so server can read them
            self.write_curr_sec()
            self.write_page_specs()
        
            # read info from server, including start/stop times of channels and re-ordering of channels
            password_needed = self.read_server_info()
            if password_needed:
                self.password = self.prompt_for_password()
                if self.password is None:
                    return  # case where user gives up, clicks 'Cancel'.  Just return from this function.
                
                continue
            self.write_curr_sec()
    
            # create channel labels based on path names
            self.channel_labels = []
            for channel in self.channels:
                new_label = channel["name"].split('/')
                self.channel_labels.append(new_label[len(new_label) - 1].split('.')[0])
        
            # read initial page
            self.read_page()
            
        
        self.read_events_from_server()
        
        # TODO: clean this up between sessions, so multiple versions of these objects aren't made?
        # buffer bar box
        self.ax_buff_box = self.buffer_bar.add_axes([0,0,1,1])
        #self.ax_buff_box.patch.set_color('blue')
        self.ax_buff_box.patch.set_color((0.6, 0.6, 0.6))  # nobel grey
        self.ax_buff_box.patch.set_x(0)
        self.ax_buff_box.patch.set_width(0)
        
        self.read_discon_from_server()
        
        # red current page box
        self.ax_cs_box = self.buffer_bar.add_axes([0,0,1,1])
        self.ax_cs_box.patch.set_color('red')
        self.ax_cs_box.patch.set_x(0)
        self.ax_cs_box.patch.set_width(0)
        
        
        if self.events is None:
            self.load_csv_annotations(self.data_dir + "/events.csv")
    
        #print(self.raw_page)
        
        # update title of main window to reflect data location
        self.setWindowTitle("EEG View - " + self.data_dir)
                
        # display initial page
        self.plot_eeg()
        
    def prompt_for_password(self):
        print("Prompting for password")
        text, ok = QInputDialog.getText(self, 'Password validation', 'Enter password for files:', QLineEdit.Password)
        if ok:
            return text
        
        return None
        
        
    def onClicked_checkbox_redraw(self):
        self.plot_eeg()
        
        
    def onClicked_buffer_bar(self, event):
        #print('%s click: button=%d, x=%d, y=%d, xdata=%f, ydata=%f' %('double' if event.dblclick else 'single', event.button, event.x, event.y, event.xdata, event.ydata))
        
        time_requested = (event.xdata * (self.session_end_time - self.session_start_time)) + self.session_start_time
        
        if time_requested > (self.session_end_time - self.secs_per_page):
            time_requested = (self.session_end_time - self.secs_per_page)
            
        if time_requested < self.session_start_time:
            time_requested = self.session_start_time
            
        self.curr_sec = int(time_requested)  # round down to nearest second
        
        self.check_for_resize()
        self.write_curr_sec()
        self.read_page()
        self.plot_eeg()
        
        
    def onClicked_resend_and_redraw(self):
        self.secs_per_page = int(self.secpage_combo.currentText())
        self.write_page_specs()
        self.reset_buffer_limits()
        self.read_page()
        self.plot_eeg()
    
        
    def get_files(self):
        file = str(QFileDialog.getExistingDirectory(self, "Select Directory"))
        return file
    

    def write_page_specs(self):
        if self.server_temp_path is None:
            return

        with open(self.server_temp_path + "page_specs", 'w') as the_file:
            the_file.write(str(random.random()) + '\n')
            the_file.write(self.data_dir  + '\n')
            the_file.write(str(self.n_displayed)  + '\n')
            
            # write absolute path for each channel
            for paths in self.channel_paths:
                the_file.write(paths  + '\n')
            
            the_file.write(str(self.axpix) + '\n')
            the_file.write(str(self.secs_per_page) + '\n')
            the_file.write("blank" + '\n')  # default password
            the_file.write("blank" + '\n')  # default events file
            the_file.close()
            
          
    # return value:  whether or not a password is needed to properly read files
    # True: password is needed, False: password is not needed.
    def read_server_info(self):
        while True:
    
            while True:
                if os.path.exists(self.server_temp_path + "password_needed"):
                    return True
                try:
                    si_file = open(self.server_temp_path + "server_info")
                    break
                except:
                    time.sleep(0.1)
                    continue
                
            lines = si_file.readlines()
            si_file.close()
            
            if (len(lines) < (self.n_displayed + 2)):
                continue
                
            break
            
        self.channels = []
            
        line_counter = 0
        self.session_start_time = -1
        self.session_end_time = -1
        while (line_counter < self.n_displayed):
            tokens = lines[line_counter+1].split()  #skip first line
            print (tokens)
            print (line_counter)
            #self.channel_paths[line_counter] = tokens[0]
            channel = dict()
            channel["name"] = tokens[0]
            channel["start_time"] = int(tokens[1])
            channel["end_time"] = int(tokens[2])
            channel["channel_number"] = int(tokens[3])
            channel["units_conversion_factor"] = float(tokens[4])
            self.channels.append(channel)
            
            # set earliest channel start time as session start time
            if self.session_start_time == -1:
                self.session_start_time = channel["start_time"] / 1000000
            else:
                if (channel["start_time"] / 1000000) < self.session_start_time:
                    self.session_start_time = channel["start_time"] / 1000000
                    
            # set latest channel end time as session end time
            if self.session_end_time == -1:
                self.session_end_time = channel["end_time"] / 1000000
            else:
                if (channel["end_time"] / 1000000) < self.session_end_time:
                    self.session_end_time = channel["end_time"] / 1000000
            
            line_counter = line_counter + 1
            
        #print ("Start", self.session_start_time, "End", self.session_end_time)
        self.curr_sec = int(self.session_start_time)
        
        return False
        
            
    def write_curr_sec(self):
            with open(self.server_temp_path + "current_sec", 'w') as the_file:
                the_file.write(str(self.curr_sec) + '\n')
                the_file.close()
        
        
    def keyUp(self):
        #self.uV_per_pixel = self.uV_per_pixel / 1.4
        self.ylim = self.ylim / 1.4
        self.plot_eeg()

    def keyDown(self):
        #self.uV_per_pixel = self.uV_per_pixel * 1.4
        self.ylim = self.ylim * 1.4
        self.plot_eeg()

    def keyLeft(self):
        self.curr_sec = self.curr_sec - self.secs_per_page
        if self.curr_sec < self.session_start_time:
            self.curr_sec = self.session_start_time
        self.curr_sec = int(self.curr_sec)
        self.check_for_resize()
        self.write_curr_sec()
        self.read_page()
        self.plot_eeg()

    def keyRight(self):
        if (self.curr_sec + self.secs_per_page) > self.session_end_time:
            return
        self.curr_sec = self.curr_sec + self.secs_per_page
        self.check_for_resize()
        self.write_curr_sec()
        self.read_page()
        self.plot_eeg()
        #self.secpage_box.clearFocus()

    def keySpace(self):
        if (self.curr_sec + self.secs_per_page) > self.session_end_time:
            return
        self.curr_sec = self.curr_sec + 1
        self.check_for_resize()
        self.write_curr_sec()
        self.read_page()
        self.plot_eeg()

    def keyPressEvent(self, event):
        #super(MainWindow, self).keyPressEvent(event)
        if event.key() == QtCore.Qt.Key_Up:
            self.keyUp()
        elif event.key() == QtCore.Qt.Key_Down:
            self.keyDown()
        elif event.key() == QtCore.Qt.Key_Left:
            self.keyLeft()
        elif event.key() == QtCore.Qt.Key_Right:
            self.keyRight()
        elif event.key() == QtCore.Qt.Key_Space:
            self.keySpace()

        
    def get_axpix(self):
        return self.axpix
        
    def plot_eeg(self):

        if self.raw_page is None:
            return
            
        num_chans = len(self.raw_page)
            
        pix_dims = self.figure.get_size_inches()
        self.ypix = round(pix_dims[1] * self.figure.dpi)
        
        # formula to set intial scaling factor
        if self.ylim < 0:
            sum_ranges = 0
            ranges_counted = 0
            for i in range(num_chans):
                #sum_ranges = sum_ranges + np.nanmax(self.raw_page[i]) - np.nanmin(self.raw_page[i])
                new_range = np.nanquantile(self.raw_page[i], 0.95) - np.nanquantile(self.raw_page[i], 0.05)
                if not np.isnan(new_range):
                    sum_ranges = sum_ranges + new_range
                    ranges_counted = ranges_counted + 1
                #print(np.nanmax(self.raw_page[i]), np.nanmin(self.raw_page[i]))
            avg_range = sum_ranges / ranges_counted
            #print (avg_range)
            self.ylim = self.ypix * (avg_range / ((self.ypix / num_chans+1) / 4))
            #print(self.ylim)
        
        chan_plot_offset = int( (self.ylim) / (num_chans + 1))
        offset = chan_plot_offset
        #scaled_page = [[0 for x in range(len(self.raw_page[0]))] for y in range(num_chans)]
    
        #scaled_page = deepcopy(self.raw_page)
        scaled_page = [np.array(row[:]) for row in self.raw_page]
    
        for i in range(num_chans):
            mean_trc = np.nanmean(scaled_page[i])
            if np.isnan(mean_trc):
               mean_trc = 0
            #print("mean:", mean_trc)
            
            # using numpy here to offset channel seems a lot faster than list comprehension
            # (more than a hundred times faster, in my testing)
            #scaled_page[i] = [x + offset - mean_trc for x in scaled_page[i]]
            if not self.reverse_voltage.isChecked():
                #mean_trc = 0 - mean_trc
                scaled_page[i] = (0 - scaled_page[i]) + offset + mean_trc
            else:
                scaled_page[i] = scaled_page[i] + offset - mean_trc

            offset = offset + chan_plot_offset

        #print(scaled_page)
        
        #X = np.linspace(self.curr_sec, self.curr_sec + self.secs_per_page, self.axpix)
        X = np.linspace(0, self.secs_per_page, self.axpix)
                
        #print("Figure dims: ", self.figure.get_size_inches(),self.figure.dpi)
        self.figure.clear()
        ax = self.figure.add_subplot(111)
        widths = [1, 5]
        #gs = self.figure.add_gridspec(1, 2, wspace=0, width_ratios=widths)
        #axs = gs.subplots(sharex=True, sharey=True)
        #ax_label = axs[0]
        #ax = axs[1]
        #ax = self.figure()
        self.figure.tight_layout()
        offset = chan_plot_offset
        counter = 0
        #plot_color = None
        plot_color = ((0, 0.443, 0.741))  # a slightly darker blue than the matplotlib default
        for i in range(len(self.raw_page)):
            if self.multicolor.isChecked():
                p = ax.plot(X, scaled_page[i], linewidth=0.75)
                ax.text(0, offset, self.channel_labels[counter] + " ", color=p[0].get_color(), ha='right')
            else:
                if plot_color is None:
                    p = ax.plot(X, scaled_page[i], linewidth=0.75)
                    plot_color = p[0].get_color()
                else:
                    p = ax.plot(X, scaled_page[i], linewidth=0.75, color=plot_color)
                ax.text(0, offset, self.channel_labels[counter] + " ", color=plot_color, ha='right')
            
            #p.get_color()
            #ax.text(0, offset, self.channel_labels[counter] + " ", color=p[0].get_color(), ha='right')
            offset = offset + chan_plot_offset
            counter = counter + 1

       
        
        ax.set_ylim(0,self.ylim)
        ax.set_ylim(ax.get_ylim()[::-1])
        #ax.set_xlim(0,self.axpix)
        ax.set_xlim(0, 0 + self.secs_per_page)
        #ax.ticklabel_format(useOffset=False)
        #ax.ticklabel_format(useOffset=False, style='plain')
        ax.ticklabel_format(style='plain')
        #ax.set_xlabel('time (s)')  #this gets cut off at the bottom with tight_layout in effect
        #ax.get_xaxis().set_visible(False)
        ax.get_yaxis().set_visible(False)
        #ax.set_facecolor((.831, .905, .831))  # RGB 212, 231, 212, this is a light green
                                              # (.831, .902, .831) is RGB 212, 230, 212
        ax.set_facecolor((1.0, 1.0, 1.0)) # white
        
        #ax_label.set_ylim(0, self.ylim)
        #ax_label.set_ylim(ax_label.get_ylim()[::-1])
        #ax_label.set_xlim(self.curr_sec, self.curr_sec + self.secs_per_page)
        #ax_label.get_xaxis().set_visible(False)
        #ax_label.get_yaxis().set_visible(False)
        
        
        plt.margins(0, 0)
        
        # draw events
        if self.hide_annotations.isChecked() == False and self.events is not None:
            #props = dict(boxstyle='round', facecolor='wheat', alpha=0.5)  #from matplotlib.org
            props = dict(boxstyle='round', facecolor='wheat')
            for et in self.events:
                if self.curr_sec < et['start'] and et['start'] < self.curr_sec + self.secs_per_page:
                    #print("***** " + str(et['start']) + et['text'])
                    #print("******* on this page *******")
                    event_x_value = et['start'] - self.curr_sec
                    ax.plot([event_x_value, event_x_value], [0, self.ylim], 'k-', lw=1)
                    event_y_value = int(self.ylim * .05)
                    #event_y_value = 100
                    ax.text(event_x_value, event_y_value, et['text'], bbox=props)
                    #print("******* on this page ******* " + str(event_y_value))
        
        
        self.canvas.draw()
        
        #self.time_label.setText(str(self.curr_sec))
        self.curr_time_label.setText("Time: " + datetime.fromtimestamp(self.curr_sec).strftime("%m/%d/%Y %H:%M:%S"))
        
        uvcm = self.ylim / ((self.ypix / self.figure.dpi ) * 2.54)
        uvcm_display = "%.4f" % uvcm
        self.uvcm_label.setText("\u03BCV/cm: " + uvcm_display.ljust(8))
        
        self.updateBufferStatus()


    
    def updateBufferStatus(self):
        
        if (self.session_start_time is None) or (self.session_end_time is None):
            return
            
        # update buffer progress bar
        progress = (self.curr_sec - self.session_start_time) / (self.session_end_time - self.session_start_time)
        page_width = self.secs_per_page / (self.session_end_time - self.session_start_time)
        # make sure width of marker is always at least a few pixels, so we can see it
        min_page_width = 4 / self.axpix
        if page_width < min_page_width:
            page_width = min_page_width
        #self.buffer_progress.setValue(progress * 100)
        
        #self.ax_cs_box.patch.set_x(progress)
        #self.ax_cs_box.patch.set_width(page_width)
        
        buffer_progress = (self.buffer_start_sec - self.session_start_time) / (self.session_end_time - self.session_start_time)
        buffer_width = ((self.buffer_end_sec + self.secs_per_page) - self.buffer_start_sec) / (self.session_end_time - self.session_start_time)
        
        # draw buffer marker as a darker grey box
        self.ax_buff_box.patch.set_x(buffer_progress)
        self.ax_buff_box.patch.set_width(buffer_width)
        #print("*****", buffer_progress, buffer_width, self.buffer_start_sec, self.session_start_time)
        
        # draw red marker for current screen position
        self.ax_cs_box.patch.set_x(progress)
        self.ax_cs_box.patch.set_width(page_width)
        
        self.canvas_buffer_bar.draw()

            
            
    def check_for_resize(self):
        # TBD - this method (pix_dims * dpi) returns an axpix value that is ~146 pixels too big,
        # when tested on Mac OS using tight layout for the figure.  This isn't a huge problem, as
        # matplotlib will presumably interpolate nicely when displaying the plots.
        pix_dims = self.figure.get_size_inches()
        if self.axpix == round(pix_dims[0] * self.figure.dpi):
            return
                
        self.axpix = round(pix_dims[0] * self.figure.dpi)
        self.ypix = round(pix_dims[1] * self.figure.dpi)
        self.write_page_specs()
        self.reset_buffer_limits()
            
            

    def reset_buffer_limits(self):
        while True:
            try:
                bl_file = open(self.server_temp_path + "buffer_limits", "w")
                break
            except:
                time.sleep(0.1)
                continue
            
        bl_file.write("0.0" + "\n")
        # write negative value so we are forced to wait for valid data from the server to proceed
        bl_file.write("-" + str(self.secs_per_page) + "\n")
        bl_file.close()
        print("UI reset buffer limits")
        time.sleep(0.2)

        
    def read_page(self):

        self.raw_page = [[0 for x in range(self.axpix)] for y in range(self.n_displayed)]
    
        while True:
    
            while True:
                try:
                    bl_file = open(self.server_temp_path + "buffer_limits")
                    break
                except:
                    time.sleep(0.1)
                    continue
                
            lines = bl_file.readlines()
            bl_file.close()
        
            if len(lines) < 2:
                time.sleep(0.1)
                continue
    
            self.buffer_start_sec = float(lines[0])
            #print("***************start:", self.buffer_start_sec)
            self.buffer_end_sec = float(lines[1])
            #print("end:", self.buffer_end_sec)
        
            if (self.curr_sec < self.buffer_start_sec) or (self.curr_sec + self.secs_per_page > self.buffer_end_sec):
                time.sleep(0.5)
            else:
                break
    
    
        while True:
            try:
                pd_file = open(self.server_temp_path + "page_data")
                break
            except:
                time.sleep(0.1)
                continue
            
        curr_buff_samp = round((self.curr_sec - self.buffer_start_sec) * self.axpix / self.secs_per_page)
        #print ("*********curr_buff_samp:", curr_buff_samp)
    
        pd_file.seek(curr_buff_samp * self.n_displayed * 4, os.SEEK_SET)
    
        # read array of float values from page_data file
        arr = np.fromfile(pd_file, dtype=np.float32, count=(self.n_displayed * self.axpix))
    
        # use 'F', or Fortran-like ordering, where the first index (n_displayed) changes the fastest.
        self.raw_page = arr.reshape(self.n_displayed, self.axpix, order='F')
    
        #chan_count = 0
        #pix_count = 0
        #for x in arr:
        #    raw_page[chan_count][pix_count] = x
        #    chan_count = chan_count + 1
        #    if chan_count == self.n_displayed:
        #        chan_count = 0
        #        pix_count = pix_count + 1
        #        if pix_count == self.axpix:
        #            pix_count = 0
    
    
        #for i in range(self.n_displayed):
        #   raw_page[i] = np.array([(raw_page[i][x]) + i for x in raw_page[i]])
        #   raw_page[i] = raw_page[i].astype("float")
        #   for y in range(len(raw_page[i])):
        #        if y < 100 or y > 200:
        #            raw_page[i][y] = math.sin(y) * raw_page[i][y]
        #        else:
        #            raw_page[i][y] = np.nan
          


def main():

    sampsPerScreenPix = 1

    app = QApplication(sys.argv)
    main = MainWindow()
    
    main.show()
    
    # wait for QDialog window to close
    return_code = app.exec_()
    print ("Exiting eeg_view application...")
    
    # tell heartbeat thread that we're done
    heartbeat_flag.set()
    #time.sleep(.6)  #give it time to kill the thread  (this seems to be not needed)
    
    # exit application
    sys.exit(return_code)

if __name__ == '__main__':
    main()
