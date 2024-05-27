@echo off
set truth_file_name=%1%.img
set starting_file_name=%2%.img
set fixed_file_name=%2%_fixed.img
set identifier_file_name=%3%.txt
set /p identifier=< %identifier_file_name%
set grader_exec=.\grader.exe
set rec_exec=.\recext2fs.exe

copy /y %starting_file_name% %fixed_file_name%

%rec_exec% %fixed_file_name% %identifier%
%grader_exec% %truth_file_name% %starting_file_name% %fixed_file_name%