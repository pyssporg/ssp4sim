#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include "c_api/simulator_c_api.h"

#include <math.h>
#include <stdint.h>

typedef struct
{
    PyObject_HEAD
    ssp4sim_simulator_handle *handle;
} PySsp4simSimulator;

static int raise_status_error(const ssp4sim_status status)
{
    const char *msg = ssp4sim_last_error();
    if (msg == NULL || msg[0] == '\0')
    {
        msg = "Unknown simulator error";
    }

    if (status == SSP4SIM_STATUS_INVALID_ARGUMENT)
    {
        PyErr_SetString(PyExc_ValueError, msg);
        return -1;
    }

    if (status == SSP4SIM_STATUS_OUT_OF_MEMORY)
    {
        PyErr_SetString(PyExc_MemoryError, msg);
        return -1;
    }

    PyErr_SetString(PyExc_RuntimeError, msg);
    return -1;
}

static PyObject *status_to_none(const ssp4sim_status status)
{
    if (status != SSP4SIM_STATUS_OK)
    {
        raise_status_error(status);
        return NULL;
    }

    Py_RETURN_NONE;
}

static int parse_at_time(PyObject *object, int *has_at, uint64_t *at_ns)
{
    *has_at = 0;
    *at_ns = 0;

    if (object == NULL || object == Py_None)
    {
        return 0;
    }

    const double seconds = PyFloat_AsDouble(object);
    if (seconds == -1.0 && PyErr_Occurred())
    {
        return -1;
    }

    if (!isfinite(seconds) || seconds < 0.0 || seconds > 1.8e10)
    {
        PyErr_SetString(
            PyExc_ValueError,
            "at must be a finite, non-negative simulation time in seconds");
        return -1;
    }

    *has_at = 1;
    *at_ns = (uint64_t)(seconds * 1.0e9 + 0.5);
    return 0;
}

static int PySsp4simSimulator_init(PyObject *self, PyObject *args, PyObject *kwargs)
{
    static char *kwlist[] = {"config_path", NULL};
    const char *config_path = NULL;
    PySsp4simSimulator *sim = (PySsp4simSimulator *)self;

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "s", kwlist, &config_path))
    {
        return -1;
    }

    if (sim->handle != NULL)
    {
        ssp4sim_simulator_destroy(sim->handle);
        sim->handle = NULL;
    }

    const ssp4sim_status status = ssp4sim_simulator_create(config_path, &sim->handle);
    if (status != SSP4SIM_STATUS_OK)
    {
        return raise_status_error(status);
    }

    return 0;
}

static void PySsp4simSimulator_dealloc(PyObject *self)
{
    PySsp4simSimulator *sim = (PySsp4simSimulator *)self;
    if (sim->handle != NULL)
    {
        ssp4sim_simulator_destroy(sim->handle);
        sim->handle = NULL;
    }
    freefunc tp_free = (freefunc)PyType_GetSlot(Py_TYPE(self), Py_tp_free);
    if (tp_free != NULL)
    {
        tp_free(self);
    }
    else
    {
        PyObject_Free(self);
    }
}

static PyObject *PySsp4simSimulator_init_method(PyObject *self, PyObject *Py_UNUSED(ignored))
{
    PySsp4simSimulator *sim = (PySsp4simSimulator *)self;
    return status_to_none(ssp4sim_simulator_init(sim->handle));
}

static PyObject *PySsp4simSimulator_simulate_method(PyObject *self, PyObject *Py_UNUSED(ignored))
{
    PySsp4simSimulator *sim = (PySsp4simSimulator *)self;
    ssp4sim_status status;
    Py_BEGIN_ALLOW_THREADS
    status = ssp4sim_simulator_simulate(sim->handle);
    Py_END_ALLOW_THREADS

    return status_to_none(status);
}

static PyObject *PySsp4simSimulator_start_method(PyObject *self, PyObject *Py_UNUSED(ignored))
{
    PySsp4simSimulator *sim = (PySsp4simSimulator *)self;
    ssp4sim_status status;
    Py_BEGIN_ALLOW_THREADS
    status = ssp4sim_simulator_start(sim->handle);
    Py_END_ALLOW_THREADS

    return status_to_none(status);
}

static PyObject *PySsp4simSimulator_pause_method(PyObject *self, PyObject *args, PyObject *kwargs)
{
    static char *kwlist[] = {"at", NULL};
    PyObject *at_object = NULL;
    int has_at = 0;
    uint64_t at_ns = 0;
    PySsp4simSimulator *sim = (PySsp4simSimulator *)self;

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|O", kwlist, &at_object))
    {
        return NULL;
    }

    if (parse_at_time(at_object, &has_at, &at_ns) < 0)
    {
        return NULL;
    }

    const ssp4sim_status status = ssp4sim_simulator_pause(sim->handle, has_at, at_ns);
    return status_to_none(status);
}

static PyObject *PySsp4simSimulator_resume_method(PyObject *self, PyObject *Py_UNUSED(ignored))
{
    PySsp4simSimulator *sim = (PySsp4simSimulator *)self;
    return status_to_none(ssp4sim_simulator_resume(sim->handle));
}

static PyObject *PySsp4simSimulator_reset_method(
    PyObject *self, PyObject *Py_UNUSED(ignored))
{
    PySsp4simSimulator *sim = (PySsp4simSimulator *)self;
    ssp4sim_status status;
    Py_BEGIN_ALLOW_THREADS
    status = ssp4sim_simulator_reset(sim->handle);
    Py_END_ALLOW_THREADS
    return status_to_none(status);
}

static PyObject *PySsp4simSimulator_stop_method(PyObject *self, PyObject *args, PyObject *kwargs)
{
    static char *kwlist[] = {"at", NULL};
    PyObject *at_object = NULL;
    int has_at = 0;
    uint64_t at_ns = 0;
    PySsp4simSimulator *sim = (PySsp4simSimulator *)self;

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|O", kwlist, &at_object))
    {
        return NULL;
    }

    if (parse_at_time(at_object, &has_at, &at_ns) < 0)
    {
        return NULL;
    }

    const ssp4sim_status status = ssp4sim_simulator_stop(sim->handle, has_at, at_ns);
    return status_to_none(status);
}

static PyObject *PySsp4simSimulator_set_speed_method(PyObject *self, PyObject *args, PyObject *kwargs)
{
    static char *kwlist[] = {"speed", "at", NULL};
    double speed = 0.0;
    PyObject *at_object = NULL;
    int has_at = 0;
    uint64_t at_ns = 0;
    PySsp4simSimulator *sim = (PySsp4simSimulator *)self;

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "d|O", kwlist, &speed,
                                     &at_object))
    {
        return NULL;
    }

    if (parse_at_time(at_object, &has_at, &at_ns) < 0)
    {
        return NULL;
    }

    const ssp4sim_status status =
        ssp4sim_simulator_set_speed(sim->handle, speed, has_at, at_ns);
    return status_to_none(status);
}

static PyObject *PySsp4simSimulator_snapshot_method(PyObject *self, PyObject *args, PyObject *kwargs)
{
    static char *kwlist[] = {"path", NULL};
    PyObject *path_object = NULL;
    PySsp4simSimulator *sim = (PySsp4simSimulator *)self;

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O", kwlist, &path_object))
    {
        return NULL;
    }

    PyObject *path_bytes = NULL;
    if (!PyUnicode_FSConverter(path_object, &path_bytes))
    {
        return NULL;
    }

    ssp4sim_simulator_handle *handle = sim->handle;
    const char *path = PyBytes_AsString(path_bytes);
    ssp4sim_status status;
    Py_BEGIN_ALLOW_THREADS
    status = ssp4sim_simulator_snapshot(handle, path);
    Py_END_ALLOW_THREADS
    Py_DECREF(path_bytes);

    return status_to_none(status);
}

static PyObject *PySsp4simSimulator_restore_method(PyObject *self, PyObject *args, PyObject *kwargs)
{
    static char *kwlist[] = {"path", "start", NULL};
    PyObject *path_object = NULL;
    int start = 1;
    PySsp4simSimulator *sim = (PySsp4simSimulator *)self;

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O|p", kwlist, &path_object,
                                     &start))
    {
        return NULL;
    }

    PyObject *path_bytes = NULL;
    if (!PyUnicode_FSConverter(path_object, &path_bytes))
    {
        return NULL;
    }

    ssp4sim_simulator_handle *handle = sim->handle;
    const char *path = PyBytes_AsString(path_bytes);
    ssp4sim_status status;
    Py_BEGIN_ALLOW_THREADS
    status = ssp4sim_simulator_restore(handle, path, start);
    Py_END_ALLOW_THREADS
    Py_DECREF(path_bytes);

    return status_to_none(status);
}

static PyObject *PySsp4simSimulator_info_method(PyObject *self, PyObject *Py_UNUSED(ignored))
{
    PySsp4simSimulator *sim = (PySsp4simSimulator *)self;
    const ssp4sim_run_info info = ssp4sim_simulator_get_info(sim->handle);

    const char *message = ssp4sim_last_error();
    if (message != NULL && message[0] != '\0')
    {
        return raise_status_error(SSP4SIM_STATUS_RUNTIME_ERROR), NULL;
    }

    return Py_BuildValue(
        "{s:s,s:K,s:d}",
        "state", ssp4sim_run_state_name(info.state),
        "sim_time_ns", (unsigned long long)info.sim_time_ns,
        "speed", info.speed);
}

static PyObject *PySsp4simSimulator_wait_method(PyObject *self, PyObject *Py_UNUSED(ignored))
{
    PySsp4simSimulator *sim = (PySsp4simSimulator *)self;
    ssp4sim_status status;
    ssp4sim_run_result result = SSP4SIM_RESULT_FAILED;
    Py_BEGIN_ALLOW_THREADS
    status = ssp4sim_simulator_wait(sim->handle, &result);
    Py_END_ALLOW_THREADS

    if (status != SSP4SIM_STATUS_OK)
    {
        return raise_status_error(status), NULL;
    }

    return PyUnicode_FromString(ssp4sim_run_result_name(result));
}

static PyMethodDef PySsp4simSimulator_methods[] = {
    {
        "init",
        (PyCFunction)PySsp4simSimulator_init_method,
        METH_NOARGS,
        PyDoc_STR("Initialize the simulator.")
    },
    {
        "simulate",
        (PyCFunction)PySsp4simSimulator_simulate_method,
        METH_NOARGS,
        PyDoc_STR("Run the simulation to completion.")
    },
    {
        "start",
        (PyCFunction)PySsp4simSimulator_start_method,
        METH_NOARGS,
        PyDoc_STR("Start the simulation and block until it is running or terminal.")
    },
    {
        "pause",
        (PyCFunction)PySsp4simSimulator_pause_method,
        METH_VARARGS | METH_KEYWORDS,
        PyDoc_STR("Pause a running simulation; optional at (sim seconds) defers it.")
    },
    {
        "resume",
        (PyCFunction)PySsp4simSimulator_resume_method,
        METH_NOARGS,
        PyDoc_STR("Resume a paused simulation.")
    },
    {
        "reset",
        (PyCFunction)PySsp4simSimulator_reset_method,
        METH_NOARGS,
        PyDoc_STR("Stop any run, discard per-run state, rebuild from the creation "
                  "config, and leave the simulator initialized.")
    },
    {
        "stop",
        (PyCFunction)PySsp4simSimulator_stop_method,
        METH_VARARGS | METH_KEYWORDS,
        PyDoc_STR("Stop a running or paused simulation; optional at (sim seconds) "
                  "defers it. A no-op once stopping or terminal; a pre-run stop "
                  "still raises.")
    },
    {
        "set_speed",
        (PyCFunction)PySsp4simSimulator_set_speed_method,
        METH_VARARGS | METH_KEYWORDS,
        PyDoc_STR("Set the wall-clock speed; optional at (sim seconds) defers it.")
    },
    {
        "snapshot",
        (PyCFunction)PySsp4simSimulator_snapshot_method,
        METH_VARARGS | METH_KEYWORDS,
        PyDoc_STR("Block until a snapshot of the running simulation is written.")
    },
    {
        "restore",
        (PyCFunction)PySsp4simSimulator_restore_method,
        METH_VARARGS | METH_KEYWORDS,
        PyDoc_STR("Restore a snapshot into a fresh run; start defaults to True.")
    },
    {
        "info",
        (PyCFunction)PySsp4simSimulator_info_method,
        METH_NOARGS,
        PyDoc_STR("Return the coherent mapping of state, sim_time_ns and speed.")
    },
    {
        "wait",
        (PyCFunction)PySsp4simSimulator_wait_method,
        METH_NOARGS,
        PyDoc_STR("Wait for the run to finish and return its result.")
    },
    {NULL, NULL, 0, NULL}
};

static PyType_Slot PySsp4simSimulator_slots[] = {
    {Py_tp_doc, "SSP4SIM simulator wrapper exposing init, simulate, start, pause, "
                "resume, reset, stop, set_speed, snapshot, restore, info, and "
                "wait."},
    {Py_tp_new, (void *)PyType_GenericNew},
    {Py_tp_init, (void *)PySsp4simSimulator_init},
    {Py_tp_dealloc, (void *)PySsp4simSimulator_dealloc},
    {Py_tp_methods, (void *)PySsp4simSimulator_methods},
    {0, NULL}
};

static PyType_Spec PySsp4simSimulator_spec = {
    "py_ssp4sim.Simulator",
    sizeof(PySsp4simSimulator),
    0,
    Py_TPFLAGS_DEFAULT,
    PySsp4simSimulator_slots
};

static PyModuleDef module_def = {
    PyModuleDef_HEAD_INIT,
    "py_ssp4sim",
    "Python Limited API bindings for ssp4sim.",
    -1,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL
};

PyMODINIT_FUNC PyInit_py_ssp4sim(void)
{
    PyObject *module = PyModule_Create(&module_def);
    if (module == NULL)
    {
        return NULL;
    }

    PyObject *sim_type = PyType_FromSpec(&PySsp4simSimulator_spec);
    if (sim_type == NULL)
    {
        Py_DECREF(module);
        return NULL;
    }

    if (PyModule_AddObject(module, "Simulator", sim_type) < 0)
    {
        Py_DECREF(sim_type);
        Py_DECREF(module);
        return NULL;
    }

    return module;
}
