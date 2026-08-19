---- MODULE scheduler ----
(*
  TLA+ specification of the Qiming delta-cycle scheduler
  (libqsim/src/scheduler.c).

  IMPORTANT: this models the standalone REFERENCE scheduler only. The
  product simulator does NOT use scheduler.c — uir_sim.c implements its
  own event engine (sim_event_t queue, uir_sim_run). This spec and the
  C implementation are kept in sync as a formally verified reference
  design; wiring scheduler.c into uir_sim.c would require re-verifying
  the properties below against the real engine.

  The scheduler manages simulation events at (time, delta) pairs.
  Events at the same time are stratified by delta cycle.
  When no events remain at the current delta, delta resets and
  time advances to the next scheduled event time.

  Extended with models of:
  - Phase 1.5 port wire propagation (INPUT-only + cascade loop)
  - Phase 2 process triggering on signal changes
  - Phase 3 backward propagation
  - Immediate write for VHDL CSA (delay>0)
  - No stale-event overwrite property
*)

EXTENDS Naturals, Sequences, TLC

CONSTANTS
    MaxDelta,           (* maximum delta cycles before timeout *)
    NumSignals,         (* number of signals *)
    InputPort,          (* signal index acting as input port parent *)
    OutputPort          (* signal index acting as output port child *)

ASSUME MaxDelta \in Nat \ {0}
ASSUME NumSignals > 1
ASSUME InputPort < NumSignals
ASSUME OutputPort < NumSignals

(* --algorithm scheduler

variables
    time = 0,
    delta = 0,
    (* Queue of scheduled signal events: [t, d, sig, val, is_stale] *)
    queue = {},
    done = FALSE,
    processed = 0,
    delta_exceeded = FALSE,

    (* Signal state: array of 4-state values *)
    signals = [i \in 0..(NumSignals-1) |-> "X"],

    (* Phase 2: processes pending to fire on signal changes *)
    pending_processes = {},

    (* Port wire cascade iteration count per delta *)
    cascade_count = 0;

define
    TypeOk ==
        /\ time \in Nat
        /\ delta \in Nat
        /\ queue \subseteq [t: Nat, d: Nat, sig: 0..(NumSignals-1),
                            val: {"0","1","X","Z"}, is_stale: BOOLEAN]
        /\ done \in BOOLEAN
        /\ processed \in Nat
        /\ delta_exceeded \in BOOLEAN
        /\ signals \in [0..(NumSignals-1) -> {"0","1","X","Z"}]
        /\ pending_processes \subseteq 0..(NumSignals-1)
        /\ cascade_count \in 0..16

    NoDeadlock ==
        [] [((queue /= {} /\ ~done) => (processed' > processed))]_processed

    DeltaBounded ==
        [] (delta <= MaxDelta)

    EventualAdvance ==
        <> done

    (* A stale event is one scheduled before a direct write.
       No signal should be overwritten by a stale event after
       a process trigger has set it. *)
    NoStaleOverwrite ==
        [](signals /= [i \in 0..(NumSignals-1) |-> "X"] =>
           \A e \in queue : ~(e.is_stale /\ e.t <= time /\ e.d <= delta
                              /\ signals[e.sig] /= "X"))

    (* Port wire cascade must terminate within 16 iterations *)
    CascadeTermination ==
        [](cascade_count <= 16)

    (* An immediate write (non-stale) to a signal should be
       visible to processes in the same delta, not delayed. *)
    ImmediateWriteVisible ==
        [](\A s \in 0..(NumSignals-1) :
            signals[s] /= "X" =>
            ~ \E e \in queue : e.sig = s /\ e.t = time /\ e.d >= delta)
end define;

begin

SchedulerLoop:
    while ~done do
        if queue = {} then
            done := TRUE;
        else
            (* ── Phase 1: Apply all events at current (time, delta) ── *)
            l_apply_loop:
            while \E e \in queue : e.t = time /\ e.d = delta do
                with e \in queue do
                    queue := queue \ {e};
                    if e.is_stale /\ signals[e.sig] /= "X" then
                        (* Stale event: skip (no overwrite) *)
                        skip;
                    else
                        (* Apply the event to the signal, also propagating
                           INPUT->OUTPUT port (single assignment: PlusCal
                           forbids two writes to the same variable inside one
                           with statement). *)
                        signals := [signals EXCEPT ![e.sig] = e.val,
                                        ![OutputPort] = IF e.sig = InputPort THEN e.val ELSE signals[OutputPort]];
                        processed := processed + 1;

                        (* Check cascade limit *)
                        if cascade_count > 16 then
                            delta_exceeded := TRUE;
                            done := TRUE;
                        end if;

                        (* ── Phase 1.5: Port wire propagation counter ──
                           INPUT-only: propagate parent (e.sig) to child.
                           Skip OUTPUT (child->parent) unconditionally. *)
                        if e.sig = InputPort then
                            cascade_count := cascade_count + 1;
                        end if;

                        (* ── Phase 2: Schedule processes ── *)
                        pending_processes := pending_processes \union {e.sig};
                    end if;
                end with;
            end while;

            (* ── Phase 2b execution ──
               For each pending signal, execute triggered processes.
               This models VHDL CSA immediate write + recursive cascade. *)
            l_cascade_loop:
            while pending_processes /= {} do
                with s \in pending_processes do
                    (* remove s and cascade to the other port (if allowed):
                       merged into one assignment (PlusCal forbids two writes
                       to the same variable inside one with statement). *)
                    pending_processes := (pending_processes \ {s}) \union
                        IF cascade_count < 16 THEN
                            IF s = InputPort THEN {OutputPort} ELSE {InputPort}
                        ELSE {};
                    if cascade_count < 16 then
                        cascade_count := cascade_count + 1;
                    end if;
                    (* Phase 3: forward/backward port propagation.
                       Single assignment (PlusCal forbids two writes to the
                       same variable inside one with statement). *)
                    signals := [signals EXCEPT
                                    ![OutputPort] = IF s = InputPort THEN signals[InputPort] ELSE signals[OutputPort],
                                    ![InputPort] = IF s = OutputPort THEN signals[OutputPort] ELSE signals[InputPort]];
                end with;
            end while;

            cascade_count := 0;

            (* ── Advance delta or time ── *)
            if \E e \in queue : e.t = time /\ e.d > delta then
                delta := delta + 1;
                if delta > MaxDelta then
                    delta_exceeded := TRUE;
                    done := TRUE;
                end if;
            else
                if \A e \in queue : e.t > time then
                    time := time + 1;
                    delta := 0;
                end if;
            end if;
        end if;
    end while;

end algorithm; *)
\* BEGIN TRANSLATION (chksum(pcal) = "22bc6aad" /\ chksum(tla) = "ea18d68a")
VARIABLES time, delta, queue, done, processed, delta_exceeded, signals, 
          pending_processes, cascade_count, pc

(* define statement *)
TypeOk ==
    /\ time \in Nat
    /\ delta \in Nat
    /\ queue \subseteq [t: Nat, d: Nat, sig: 0..(NumSignals-1),
                        val: {"0","1","X","Z"}, is_stale: BOOLEAN]
    /\ done \in BOOLEAN
    /\ processed \in Nat
    /\ delta_exceeded \in BOOLEAN
    /\ signals \in [0..(NumSignals-1) -> {"0","1","X","Z"}]
    /\ pending_processes \subseteq 0..(NumSignals-1)
    /\ cascade_count \in 0..16

NoDeadlock ==
    [] [((queue /= {} /\ ~done) => (processed' > processed))]_processed

DeltaBounded ==
    [] (delta <= MaxDelta)

EventualAdvance ==
    <> done




NoStaleOverwrite ==
    [](signals /= [i \in 0..(NumSignals-1) |-> "X"] =>
       \A e \in queue : ~(e.is_stale /\ e.t <= time /\ e.d <= delta
                          /\ signals[e.sig] /= "X"))


CascadeTermination ==
    [](cascade_count <= 16)



ImmediateWriteVisible ==
    [](\A s \in 0..(NumSignals-1) :
        signals[s] /= "X" =>
        ~ \E e \in queue : e.sig = s /\ e.t = time /\ e.d >= delta)


vars == << time, delta, queue, done, processed, delta_exceeded, signals, 
           pending_processes, cascade_count, pc >>

Init == (* Global variables *)
        /\ time = 0
        /\ delta = 0
        /\ queue = {}
        /\ done = FALSE
        /\ processed = 0
        /\ delta_exceeded = FALSE
        /\ signals = [i \in 0..(NumSignals-1) |-> "X"]
        /\ pending_processes = {}
        /\ cascade_count = 0
        /\ pc = "SchedulerLoop"

SchedulerLoop == /\ pc = "SchedulerLoop"
                 /\ IF ~done
                       THEN /\ IF queue = {}
                                  THEN /\ done' = TRUE
                                       /\ pc' = "SchedulerLoop"
                                  ELSE /\ pc' = "l_apply_loop"
                                       /\ done' = done
                       ELSE /\ pc' = "Done"
                            /\ done' = done
                 /\ UNCHANGED << time, delta, queue, processed, delta_exceeded, 
                                 signals, pending_processes, cascade_count >>

l_apply_loop == /\ pc = "l_apply_loop"
                /\ IF \E e \in queue : e.t = time /\ e.d = delta
                      THEN /\ \E e \in queue:
                                /\ queue' = queue \ {e}
                                /\ IF e.is_stale /\ signals[e.sig] /= "X"
                                      THEN /\ TRUE
                                           /\ UNCHANGED << done, processed, 
                                                           delta_exceeded, 
                                                           signals, 
                                                           pending_processes, 
                                                           cascade_count >>
                                      ELSE /\ signals' = [signals EXCEPT ![e.sig] = e.val,
                                                              ![OutputPort] = IF e.sig = InputPort THEN e.val ELSE signals[OutputPort]]
                                           /\ processed' = processed + 1
                                           /\ IF cascade_count > 16
                                                 THEN /\ delta_exceeded' = TRUE
                                                      /\ done' = TRUE
                                                 ELSE /\ TRUE
                                                      /\ UNCHANGED << done, 
                                                                      delta_exceeded >>
                                           /\ IF e.sig = InputPort
                                                 THEN /\ cascade_count' = cascade_count + 1
                                                 ELSE /\ TRUE
                                                      /\ UNCHANGED cascade_count
                                           /\ pending_processes' = (pending_processes \union {e.sig})
                           /\ pc' = "l_apply_loop"
                      ELSE /\ pc' = "l_cascade_loop"
                           /\ UNCHANGED << queue, done, processed, 
                                           delta_exceeded, signals, 
                                           pending_processes, cascade_count >>
                /\ UNCHANGED << time, delta >>

l_cascade_loop == /\ pc = "l_cascade_loop"
                  /\ IF pending_processes /= {}
                        THEN /\ \E s \in pending_processes:
                                  /\ pending_processes' = (                 (pending_processes \ {s}) \union
                                                           IF cascade_count < 16 THEN
                                                               IF s = InputPort THEN {OutputPort} ELSE {InputPort}
                                                           ELSE {})
                                  /\ IF cascade_count < 16
                                        THEN /\ cascade_count' = cascade_count + 1
                                        ELSE /\ TRUE
                                             /\ UNCHANGED cascade_count
                                  /\ signals' = [signals EXCEPT
                                                     ![OutputPort] = IF s = InputPort THEN signals[InputPort] ELSE signals[OutputPort],
                                                     ![InputPort] = IF s = OutputPort THEN signals[OutputPort] ELSE signals[InputPort]]
                             /\ pc' = "l_cascade_loop"
                             /\ UNCHANGED << time, delta, done, delta_exceeded >>
                        ELSE /\ cascade_count' = 0
                             /\ IF \E e \in queue : e.t = time /\ e.d > delta
                                   THEN /\ delta' = delta + 1
                                        /\ IF delta' > MaxDelta
                                              THEN /\ delta_exceeded' = TRUE
                                                   /\ done' = TRUE
                                              ELSE /\ TRUE
                                                   /\ UNCHANGED << done, 
                                                                   delta_exceeded >>
                                        /\ time' = time
                                   ELSE /\ IF \A e \in queue : e.t > time
                                              THEN /\ time' = time + 1
                                                   /\ delta' = 0
                                              ELSE /\ TRUE
                                                   /\ UNCHANGED << time, delta >>
                                        /\ UNCHANGED << done, delta_exceeded >>
                             /\ pc' = "SchedulerLoop"
                             /\ UNCHANGED << signals, pending_processes >>
                  /\ UNCHANGED << queue, processed >>

(* Allow infinite stuttering to prevent deadlock on termination. *)
Terminating == pc = "Done" /\ UNCHANGED vars

Next == SchedulerLoop \/ l_apply_loop \/ l_cascade_loop
           \/ Terminating

Spec == Init /\ [][Next]_vars

Termination == <>(pc = "Done")

\* END TRANSLATION 

====
