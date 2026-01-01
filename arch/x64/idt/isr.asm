[bits 64]

extern irq_common_stub

; Helper macro to handle swapgs on entry if from Ring 3
%macro SWAPGS_IF_NEEDED 1
    test qword [rsp + %1], 3 ; Check CS
    jz %%no_swapgs_entry
    swapgs
%%no_swapgs_entry:
%endmacro

; Helper macro to handle swapgs on exit if returning to Ring 3
%macro SWAPGS_IF_NEEDED_EXIT 1
    test qword [rsp + %1], 3 ; Check CS
    jz %%no_swapgs_exit
    swapgs
%%no_swapgs_exit:
%endmacro

%macro ISR_NOERRCODE 1
section .text
global isr%1
isr%1:
    ; CPU pushes: RIP, CS, RFLAGS, RSP, SS
    
    SWAPGS_IF_NEEDED 8

    ; For consistency with ISR_ERRCODE, push a dummy error code
    push qword 0
    ; Push interrupt number
    push qword %1
    ; Push general purpose registers (in reverse order of struct for convenience)
    push r15
    push r14
    push r13
    push r12
    push r11
    push r10
    push r9
    push r8
    push rbp
    push rsi
    push rdi
    push rdx
    push rcx
    push rbx
    push rax
    ; Push segment registers (in reverse order of struct: gs, fs, es, ds)
    mov rax, gs
    push rax
    mov rax, fs
    push rax
    mov rax, es
    push rax
    mov rax, ds
    push rax

    ; Load kernel data segments
    mov ax, 0x10
    mov ds, ax
    mov es, ax

    ; The pointer to the Registers struct should be RSP
    mov rdi, rsp
    call irq_common_stub

    ; Skip reloading segments from stack (preserves GS base)
    add rsp, 32
    ; Pop general purpose registers
    pop rax
    pop rbx
    pop rcx
    pop rdx
    pop rdi
    pop rsi
    pop rbp
    pop r8
    pop r9
    pop r10
    pop r11
    pop r12
    pop r13
    pop r14
    pop r15

    ; Pop interrupt number and dummy error code
    add rsp, 16

    SWAPGS_IF_NEEDED_EXIT 8

    iretq
%endmacro

%macro ISR_ERRCODE 1
section .text
global isr%1
isr%1:
    ; CPU pushes: Error Code, RIP, CS, RFLAGS, RSP, SS
    
    SWAPGS_IF_NEEDED 16

    ; Push interrupt number
    push qword %1
    ; Push general purpose registers
    push r15
    push r14
    push r13
    push r12
    push r11
    push r10
    push r9
    push r8
    push rbp
    push rsi
    push rdi
    push rdx
    push rcx
    push rbx
    push rax
    ; Push segment registers (in reverse order of struct: gs, fs, es, ds)
    mov rax, gs
    push rax
    mov rax, fs
    push rax
    mov rax, es
    push rax
    mov rax, ds
    push rax

    ; Load kernel data segments
    mov ax, 0x10
    mov ds, ax
    mov es, ax

    ; The pointer to the Registers struct should be RSP
    mov rdi, rsp
    call irq_common_stub

    ; Skip reloading segments from stack (preserves GS base)
    add rsp, 32
    ; Pop general purpose registers
    pop rax
    pop rbx
    pop rcx
    pop rdx
    pop rdi
    pop rsi
    pop rbp
    pop r8
    pop r9
    pop r10
    pop r11
    pop r12
    pop r13
    pop r14
    pop r15

    ; Pop interrupt number
    add rsp, 8

    SWAPGS_IF_NEEDED_EXIT 16

    ; Pop error code
    add rsp, 8

    iretq
%endmacro

ISR_NOERRCODE 0
ISR_NOERRCODE 1
ISR_NOERRCODE 2
ISR_NOERRCODE 3
ISR_NOERRCODE 4
ISR_NOERRCODE 5
ISR_NOERRCODE 6
ISR_NOERRCODE 7
ISR_ERRCODE 8
ISR_NOERRCODE 9
ISR_ERRCODE 10
ISR_ERRCODE 11
ISR_ERRCODE 12
ISR_ERRCODE 13
ISR_ERRCODE 14
ISR_NOERRCODE 15
ISR_NOERRCODE 16
ISR_ERRCODE 17
ISR_NOERRCODE 18
ISR_NOERRCODE 19
ISR_NOERRCODE 20
ISR_NOERRCODE 21
ISR_NOERRCODE 22
ISR_NOERRCODE 23
ISR_NOERRCODE 24
ISR_NOERRCODE 25
ISR_NOERRCODE 26
ISR_NOERRCODE 27
ISR_NOERRCODE 28
ISR_NOERRCODE 29
ISR_NOERRCODE 30
ISR_NOERRCODE 31

; IRQs (Interrupt Requests) - these are external interrupts
ISR_NOERRCODE 32
ISR_NOERRCODE 33
ISR_NOERRCODE 34
ISR_NOERRCODE 35
ISR_NOERRCODE 36
ISR_NOERRCODE 37
ISR_NOERRCODE 38
ISR_NOERRCODE 39
ISR_NOERRCODE 40
ISR_NOERRCODE 41
ISR_NOERRCODE 42
ISR_NOERRCODE 43
ISR_NOERRCODE 44
ISR_NOERRCODE 45
ISR_NOERRCODE 46
ISR_NOERRCODE 47

; Remaining ISRs (48-255) - generic, no error code
ISR_NOERRCODE 48
ISR_NOERRCODE 49
ISR_NOERRCODE 50
ISR_NOERRCODE 51
ISR_NOERRCODE 52
ISR_NOERRCODE 53
ISR_NOERRCODE 54
ISR_NOERRCODE 55
ISR_NOERRCODE 56
ISR_NOERRCODE 57
ISR_NOERRCODE 58
ISR_NOERRCODE 59
ISR_NOERRCODE 60
ISR_NOERRCODE 61
ISR_NOERRCODE 62
ISR_NOERRCODE 63
ISR_NOERRCODE 64
ISR_NOERRCODE 65
ISR_NOERRCODE 66
ISR_NOERRCODE 67
ISR_NOERRCODE 68
ISR_NOERRCODE 69
ISR_NOERRCODE 70
ISR_NOERRCODE 71
ISR_NOERRCODE 72
ISR_NOERRCODE 73
ISR_NOERRCODE 74
ISR_NOERRCODE 75
ISR_NOERRCODE 76
ISR_NOERRCODE 77
ISR_NOERRCODE 78
ISR_NOERRCODE 79
ISR_NOERRCODE 80
ISR_NOERRCODE 81
ISR_NOERRCODE 82
ISR_NOERRCODE 83
ISR_NOERRCODE 84
ISR_NOERRCODE 85
ISR_NOERRCODE 86
ISR_NOERRCODE 87
ISR_NOERRCODE 88
ISR_NOERRCODE 89
ISR_NOERRCODE 90
ISR_NOERRCODE 91
ISR_NOERRCODE 92
ISR_NOERRCODE 93
ISR_NOERRCODE 94
ISR_NOERRCODE 95
ISR_NOERRCODE 96
ISR_NOERRCODE 97
ISR_NOERRCODE 98
ISR_NOERRCODE 99
ISR_NOERRCODE 100
ISR_NOERRCODE 101
ISR_NOERRCODE 102
ISR_NOERRCODE 103
ISR_NOERRCODE 104
ISR_NOERRCODE 105
ISR_NOERRCODE 106
ISR_NOERRCODE 107
ISR_NOERRCODE 108
ISR_NOERRCODE 109
ISR_NOERRCODE 110
ISR_NOERRCODE 111
ISR_NOERRCODE 112
ISR_NOERRCODE 113
ISR_NOERRCODE 114
ISR_NOERRCODE 115
ISR_NOERRCODE 116
ISR_NOERRCODE 117
ISR_NOERRCODE 118
ISR_NOERRCODE 119
ISR_NOERRCODE 120
ISR_NOERRCODE 121
ISR_NOERRCODE 122
ISR_NOERRCODE 123
ISR_NOERRCODE 124
ISR_NOERRCODE 125
ISR_NOERRCODE 126
ISR_NOERRCODE 127
ISR_NOERRCODE 128
ISR_NOERRCODE 129
ISR_NOERRCODE 130
ISR_NOERRCODE 131
ISR_NOERRCODE 132
ISR_NOERRCODE 133
ISR_NOERRCODE 134
ISR_NOERRCODE 135
ISR_NOERRCODE 136
ISR_NOERRCODE 137
ISR_NOERRCODE 138
ISR_NOERRCODE 139
ISR_NOERRCODE 140
ISR_NOERRCODE 141
ISR_NOERRCODE 142
ISR_NOERRCODE 143
ISR_NOERRCODE 144
ISR_NOERRCODE 145
ISR_NOERRCODE 146
ISR_NOERRCODE 147
ISR_NOERRCODE 148
ISR_NOERRCODE 149
ISR_NOERRCODE 150
ISR_NOERRCODE 151
ISR_NOERRCODE 152
ISR_NOERRCODE 153
ISR_NOERRCODE 154
ISR_NOERRCODE 155
ISR_NOERRCODE 156
ISR_NOERRCODE 157
ISR_NOERRCODE 158
ISR_NOERRCODE 159
ISR_NOERRCODE 160
ISR_NOERRCODE 161
ISR_NOERRCODE 162
ISR_NOERRCODE 163
ISR_NOERRCODE 164
ISR_NOERRCODE 165
ISR_NOERRCODE 166
ISR_NOERRCODE 167
ISR_NOERRCODE 168
ISR_NOERRCODE 169
ISR_NOERRCODE 170
ISR_NOERRCODE 171
ISR_NOERRCODE 172
ISR_NOERRCODE 173
ISR_NOERRCODE 174
ISR_NOERRCODE 175
ISR_NOERRCODE 176
ISR_NOERRCODE 177
ISR_NOERRCODE 178
ISR_NOERRCODE 179
ISR_NOERRCODE 180
ISR_NOERRCODE 181
ISR_NOERRCODE 182
ISR_NOERRCODE 183
ISR_NOERRCODE 184
ISR_NOERRCODE 185
ISR_NOERRCODE 186
ISR_NOERRCODE 187
ISR_NOERRCODE 188
ISR_NOERRCODE 189
ISR_NOERRCODE 190
ISR_NOERRCODE 191
ISR_NOERRCODE 192
ISR_NOERRCODE 193
ISR_NOERRCODE 194
ISR_NOERRCODE 195
ISR_NOERRCODE 196
ISR_NOERRCODE 197
ISR_NOERRCODE 198
ISR_NOERRCODE 199
ISR_NOERRCODE 200
ISR_NOERRCODE 201
ISR_NOERRCODE 202
ISR_NOERRCODE 203
ISR_NOERRCODE 204
ISR_NOERRCODE 205
ISR_NOERRCODE 206
ISR_NOERRCODE 207
ISR_NOERRCODE 208
ISR_NOERRCODE 209
ISR_NOERRCODE 210
ISR_NOERRCODE 211
ISR_NOERRCODE 212
ISR_NOERRCODE 213
ISR_NOERRCODE 214
ISR_NOERRCODE 215
ISR_NOERRCODE 216
ISR_NOERRCODE 217
ISR_NOERRCODE 218
ISR_NOERRCODE 219
ISR_NOERRCODE 220
ISR_NOERRCODE 221
ISR_NOERRCODE 222
ISR_NOERRCODE 223
ISR_NOERRCODE 224
ISR_NOERRCODE 225
ISR_NOERRCODE 226
ISR_NOERRCODE 227
ISR_NOERRCODE 228
ISR_NOERRCODE 229
ISR_NOERRCODE 230
ISR_NOERRCODE 231
ISR_NOERRCODE 232
ISR_NOERRCODE 233
ISR_NOERRCODE 234
ISR_NOERRCODE 235
ISR_NOERRCODE 236
ISR_NOERRCODE 237
ISR_NOERRCODE 238
ISR_NOERRCODE 239
ISR_NOERRCODE 240
ISR_NOERRCODE 241
ISR_NOERRCODE 242
ISR_NOERRCODE 243
ISR_NOERRCODE 244
ISR_NOERRCODE 245
ISR_NOERRCODE 246
ISR_NOERRCODE 247
ISR_NOERRCODE 248
ISR_NOERRCODE 249
ISR_NOERRCODE 250
ISR_NOERRCODE 251
ISR_NOERRCODE 252
ISR_NOERRCODE 253
ISR_NOERRCODE 254
ISR_NOERRCODE 255

section .rodata
global isr_stub_table
isr_stub_table:
dq isr0
dq isr1
dq isr2
dq isr3
dq isr4
dq isr5
dq isr6
dq isr7
dq isr8
dq isr9
dq isr10
dq isr11
dq isr12
dq isr13
dq isr14
dq isr15
dq isr16
dq isr17
dq isr18
dq isr19
dq isr20
dq isr21
dq isr22
dq isr23
dq isr24
dq isr25
dq isr26
dq isr27
dq isr28
dq isr29
dq isr30
dq isr31

; IRQs (Interrupt Requests) - these are external interrupts
dq isr32
dq isr33
dq isr34
dq isr35
dq isr36
dq isr37
dq isr38
dq isr39
dq isr40
dq isr41
dq isr42
dq isr43
dq isr44
dq isr45
dq isr46
dq isr47

; Remaining ISRs (48-255) - generic, no error code
dq isr48
dq isr49
dq isr50
dq isr51
dq isr52
dq isr53
dq isr54
dq isr55
dq isr56
dq isr57
dq isr58
dq isr59
dq isr60
dq isr61
dq isr62
dq isr63
dq isr64
dq isr65
dq isr66
dq isr67
dq isr68
dq isr69
dq isr70
dq isr71
dq isr72
dq isr73
dq isr74
dq isr75
dq isr76
dq isr77
dq isr78
dq isr79
dq isr80
dq isr81
dq isr82
dq isr83
dq isr84
dq isr85
dq isr86
dq isr87
dq isr88
dq isr89
dq isr90
dq isr91
dq isr92
dq isr93
dq isr94
dq isr95
dq isr96
dq isr97
dq isr98
dq isr99
dq isr100
dq isr101
dq isr102
dq isr103
dq isr104
dq isr105
dq isr106
dq isr107
dq isr108
dq isr109
dq isr110
dq isr111
dq isr112
dq isr113
dq isr114
dq isr115
dq isr116
dq isr117
dq isr118
dq isr119
dq isr120
dq isr121
dq isr122
dq isr123
dq isr124
dq isr125
dq isr126
dq isr127
dq isr128
dq isr129
dq isr130
dq isr131
dq isr132
dq isr133
dq isr134
dq isr135
dq isr136
dq isr137
dq isr138
dq isr139
dq isr140
dq isr141
dq isr142
dq isr143
dq isr144
dq isr145
dq isr146
dq isr147
dq isr148
dq isr149
dq isr150
dq isr151
dq isr152
dq isr153
dq isr154
dq isr155
dq isr156
dq isr157
dq isr158
dq isr159
dq isr160
dq isr161
dq isr162
dq isr163
dq isr164
dq isr165
dq isr166
dq isr167
dq isr168
dq isr169
dq isr170
dq isr171
dq isr172
dq isr173
dq isr174
dq isr175
dq isr176
dq isr177
dq isr178
dq isr179
dq isr180
dq isr181
dq isr182
dq isr183
dq isr184
dq isr185
dq isr186
dq isr187
dq isr188
dq isr189
dq isr190
dq isr191
dq isr192
dq isr193
dq isr194
dq isr195
dq isr196
dq isr197
dq isr198
dq isr199
dq isr200
dq isr201
dq isr202
dq isr203
dq isr204
dq isr205
dq isr206
dq isr207
dq isr208
dq isr209
dq isr210
dq isr211
dq isr212
dq isr213
dq isr214
dq isr215
dq isr216
dq isr217
dq isr218
dq isr219
dq isr220
dq isr221
dq isr222
dq isr223
dq isr224
dq isr225
dq isr226
dq isr227
dq isr228
dq isr229
dq isr230
dq isr231
dq isr232
dq isr233
dq isr234
dq isr235
dq isr236
dq isr237
dq isr238
dq isr239
dq isr240
dq isr241
dq isr242
dq isr243
dq isr244
dq isr245
dq isr246
dq isr247
dq isr248
dq isr249
dq isr250
dq isr251
dq isr252
dq isr253
dq isr254
dq isr255
