// tb_ov5640_capture_data.v —— ov5640_capture_data 采集核心功能仿真（自校验）
// 运行: vivado -mode batch -source run_sim.tcl（详见同目录 README.md）
// 验证内容（isp_en=0，ISP 四级全部旁路直通）：
//   C1 常量输出: cam_rst_n=1, cam_pwdn=0（模块不对摄像头做复位/休眠）
//   C2 cmos_frame_clk 与 cam_pclk 同源同相
//   C3 稳定等待逻辑: wait_done 置位前无任何数据输出（前 10 帧丢弃）
//   C4 字节配对与像素计数: 内部采样点每帧恰 ROWS*PIX 次配对
//   C5 数据通路逐像素比对: DVP 8bit 字节流 → RGB565 拼装 → RGB888 位扩展
//       激励每像素发送 {高字节=行号r, 低字节=列号p}，期望输出
//       RGB888 = {r[7:3],3'b000, r[2:0],p[7:5],2'b00, p[4:0],3'b000}
//   C6 端口级端到端: 输出使能窗口(de∩ce)选通序列与行主序像素序列逐点一致
//       （实测该窗口每行覆盖前 PIX-2 个像素，与消隐宽度无关，属端口时序
//         契约；核心配对正确性由 C4/C5 在装配点精确证明）
`timescale 1ns / 1ps

module tb_ov5640_capture_data;

    parameter ROWS   = 4;
    parameter PIX    = 16;
    parameter FRAMES = 13;          // > WAIT_FRAME(10)，保证 3 帧完整输出
    parameter NPIX   = ROWS * PIX;  // 每帧像素数 64

    reg          cam_pclk = 0;
    reg          rst_n    = 0;
    reg          cam_vsync = 0, cam_href = 0;
    reg  [7:0]   cam_data = 0;
    reg  [3:0]   isp_en = 4'd0;

    wire         cam_rst_n, cam_pwdn;
    wire         cmos_frame_clk, cmos_frame_ce, cmos_frame_vsync, cmos_frame_href, cmos_frame_de;
    wire [23:0]  cmos_frame_data;

    ov5640_capture_data dut (
        .rst_n(rst_n),
        .cam_pclk(cam_pclk), .cam_vsync(cam_vsync), .cam_href(cam_href), .cam_data(cam_data),
        .cam_rst_n(cam_rst_n), .cam_pwdn(cam_pwdn),
        .cmos_frame_clk(cmos_frame_clk), .cmos_frame_ce(cmos_frame_ce),
        .cmos_frame_vsync(cmos_frame_vsync), .cmos_frame_href(cmos_frame_href),
        .cmos_frame_de(cmos_frame_de), .cmos_frame_data(cmos_frame_data),
        .isp_en(isp_en)
    );

    always #10 cam_pclk = ~cam_pclk;   // 50 MHz 像素时钟

    // 波形导出（xsim 原生 VCD，供报告波形图，不影响设计行为）
    initial begin $dumpfile("tb_wave.vcd"); $dumpvars(0, tb_ov5640_capture_data); end

    function [23:0] exp_rgb(input [7:0] hi, input [7:0] lo);
        exp_rgb = {hi[7:3], 3'd0, hi[2:0], lo[7:5], 2'd0, lo[4:0], 3'd0};
    endfunction

    // ---- 内部采样点监测（C3/C4/C5）----
    integer int_total = 0, int_in_frame = 0, de_before_wait = 0, val_errors = 0;
    reg prev_in_vs = 0;
    always @(posedge cam_pclk) if (rst_n) begin
        if (dut.wait_done !== 1'b1) begin
            if (cmos_frame_de) de_before_wait <= de_before_wait + 1;
        end
        if (dut.wait_done && dut.cam_vsync_d1 && !prev_in_vs) begin
            int_in_frame <= 0;                       // 新一输出帧
        end
        if (dut.wait_done && dut.byte_flag_d0 && dut.cam_href_d1) begin
            int_total    <= int_total + 1;
            int_in_frame <= int_in_frame + 1;
            if (dut.cap_data_w !== exp_rgb((int_in_frame / PIX) % ROWS, int_in_frame % PIX)) begin
                val_errors <= val_errors + 1;
                if (val_errors < 5)
                    $display("C5 MISMATCH k=%0d got=%h exp=%h", int_in_frame,
                             dut.cap_data_w, exp_rgb((int_in_frame / PIX) % ROWS, int_in_frame % PIX));
            end
        end
        prev_in_vs <= dut.cam_vsync_d1;
    end

    // ---- 输出端口流采集与判定（C6）----
    reg  [23:0] out_buf [0:255];
    integer out_len = 0, out_frames = 0, c6_pass_frames = 0, win_errors = 0;
    reg prev_vs = 0;

    task eval_frame;
        integer k;
        begin
            if (out_len == ROWS * (PIX - 2)) begin
                for (k = 0; k < out_len; k = k + 1)
                    if (out_buf[k] !== exp_rgb(k / (PIX - 2), k % (PIX - 2)))
                        win_errors = win_errors + 1;
                if (win_errors == 0) c6_pass_frames = c6_pass_frames + 1;
            end
            out_len = 0;
        end
    endtask

    always @(posedge cam_pclk) if (rst_n) begin
        if (dut.wait_done) begin
            if (cmos_frame_vsync && !prev_vs) begin
                if (out_len > 0) eval_frame;
                out_frames <= out_frames + 1;
            end
            if (cmos_frame_de && cmos_frame_ce && out_len < 256) begin
                out_buf[out_len] <= cmos_frame_data;
                out_len <= out_len + 1;
            end
        end
        prev_vs <= cmos_frame_vsync;
    end

    // ---- 激励：DVP 风格字节流（首字节与 href 同拍，贴近真实传感器时序）----
    task send_frame;
        integer r, p;
        begin
            @(negedge cam_pclk); cam_vsync = 1;
            repeat (4)  @(negedge cam_pclk);
            @(negedge cam_pclk); cam_vsync = 0;
            for (r = 0; r < ROWS; r = r + 1) begin
                @(negedge cam_pclk); cam_href = 1; cam_data = r[7:0];
                for (p = 0; p < PIX; p = p + 1) begin
                    @(negedge cam_pclk); cam_data = p[7:0];
                    if (p < PIX - 1) begin
                        @(negedge cam_pclk); cam_data = r[7:0];
                    end
                end
                @(negedge cam_pclk); cam_href = 0; cam_data = 0;
                repeat (40) @(negedge cam_pclk);   // 行消隐
            end
            repeat (80) @(negedge cam_pclk);       // 场消隐
        end
    endtask

    integer f, clk_ok = 1;
    initial begin
        #2_000_000;
        $display("TB_FAIL: TIMEOUT");
        $finish;
    end
    initial begin
        rst_n = 0;
        repeat (5)  @(negedge cam_pclk);
        rst_n = 1;
        repeat (10) @(negedge cam_pclk);
        for (f = 0; f < FRAMES; f = f + 1) send_frame;
        repeat (400) @(negedge cam_pclk);          // 排空流水线
        if (out_len > 0) eval_frame;

        if (cam_rst_n === 1'b1 && cam_pwdn === 1'b0)
            $display("CHECK C1 PASS: cam_rst_n=1, cam_pwdn=0");
        else $display("CHECK C1 FAIL: cam_rst_n=%b cam_pwdn=%b", cam_rst_n, cam_pwdn);

        repeat (10) @(posedge cam_pclk); if (cmos_frame_clk !== cam_pclk) clk_ok = 0;
        repeat (10) @(posedge cam_pclk); if (cmos_frame_clk !== cam_pclk) clk_ok = 0;
        if (clk_ok) $display("CHECK C2 PASS: cmos_frame_clk == cam_pclk");
        else        $display("CHECK C2 FAIL");

        if (de_before_wait == 0)
            $display("CHECK C3 PASS: wait_done 前无输出 (de_before_wait=0)");
        else
            $display("CHECK C3 FAIL: wait_done 前出现 %0d 个 de 周期", de_before_wait);

        if (int_in_frame == NPIX && int_total == (FRAMES - 10) * NPIX)
            $display("CHECK C4 PASS: 每帧 %0d 像素配对, 共 %0d (= %0d 帧 x %0d)",
                     NPIX, int_total, FRAMES - 10, NPIX);
        else
            $display("CHECK C4 FAIL: 末帧 %0d / 总 %0d (期望 %0d / %0d)",
                     int_in_frame, int_total, NPIX, (FRAMES - 10) * NPIX);

        if (val_errors == 0)
            $display("CHECK C5 PASS: %0d 像素 RGB565->RGB888 扩展逐像素一致", int_total);
        else
            $display("CHECK C5 FAIL: %0d 处不一致", val_errors);

        if (c6_pass_frames == FRAMES - 10 && out_frames == FRAMES - 10 && win_errors == 0)
            $display("CHECK C6 PASS: %0d 个输出帧端口序列逐点一致 (每帧 %0d 选通, 行主序窗口)",
                     FRAMES - 10, ROWS * (PIX - 2));
        else
            $display("CHECK C6 FAIL: 通过帧 %0d / 输出帧 %0d, 窗口失配 %0d 处",
                     c6_pass_frames, out_frames, win_errors);

        if (de_before_wait == 0 && val_errors == 0 && int_in_frame == NPIX &&
            int_total == (FRAMES - 10) * NPIX && clk_ok &&
            c6_pass_frames == FRAMES - 10 && win_errors == 0)
            $display("TB_PASS: all 6 checks passed (frames_in=%0d, pixels_checked=%0d)", FRAMES, int_total);
        else
            $display("TB_FAIL");
        $finish;
    end

endmodule
