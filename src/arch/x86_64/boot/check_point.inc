; Definition of check_point structure for assembly code
struc check_point
    .success_word:      resd 1      ; uint32_t
    .failure_flags:     resb 1      ; uint8_t
    .failure_caused_excption_num: resb 1  ; uint8_t
    .failure_final_stack_top:     resw 1  ; uint16_t
    .check_point_id:              resw 1  ; uint16_t
    .reserved:                   resw 3  ; uint16_t[3]
endstruc

; Size of check_point structure
%assign check_point_size 16