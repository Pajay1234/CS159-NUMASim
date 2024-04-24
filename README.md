For creating your own instructions for a task, 
0xAA AB BC DD
A = block id(different for code and in machine sorta kinda)
 - controls which data segment to perform the operation on
B = offset
 - offset in data segment
C = operation (limited to add, sub, div, mult, mod)
 - refer to executeTask() to look at specific opcodes
D = constant
 - constant to perform arithmetic operation with selected memory location
